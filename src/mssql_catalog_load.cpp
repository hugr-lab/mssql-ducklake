#include "mssql_metadata_manager.hpp"
#include "mssql_metadata_internal.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_map>

#include "common/ducklake_util.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

// The catalog load (specs/020). DuckLake rebuilds its catalog at a snapshot from fifteen tables in
// seven queries (BuildCatalogForSnapshot); through the catalog path every one of them is several
// scans joined on the client, each a round trip. Here the catalog's rows come from one statement
// the server answers alone - a UNION ALL of one branch per table, in one tagged shape, each
// versioned row with its begin and end snapshot - and the client joins, filters and orders them
// into the structure the base builds (phase A).
//
// The same rows are kept per attached catalog (phase B, the mirror): every row of the catalog's
// history once, then only what changed since, in one small statement. A rebuild at a snapshot the
// mirror has caught up to reads nothing - which is every rebuild a flush or a compaction makes, one
// per table, each at the schema version that table was written at. Committed history does not
// change: a row's begin_snapshot is written by the commit that inserts it, its end_snapshot once by
// the commit that ends it. So a mirror is never wrong, only behind, and behind costs one catch-up.

namespace {

enum CatalogRowKind : int64_t {
	SCHEMA_ROW = 1,
	TABLE_ROW = 2,
	COLUMN_ROW = 3,
	TAG_ROW = 4,
	COLUMN_TAG_ROW = 5,
	INLINED_TABLE_ROW = 6,
	VIEW_ROW = 7,
	VIEW_COLUMN_TAG_ROW = 8,
	MACRO_ROW = 9,
	MACRO_IMPL_ROW = 10,
	MACRO_PARAMETER_ROW = 11,
	PARTITION_ROW = 12,
	PARTITION_COLUMN_ROW = 13,
	SORT_ROW = 14,
	SORT_EXPRESSION_ROW = 15,
	META_ROW = 16
};

//! the kinds that carry begin_snapshot/end_snapshot; the rest belong to a versioned parent
bool IsVersioned(int64_t kind) {
	switch (kind) {
	case SCHEMA_ROW:
	case TABLE_ROW:
	case COLUMN_ROW:
	case TAG_ROW:
	case COLUMN_TAG_ROW:
	case VIEW_ROW:
	case VIEW_COLUMN_TAG_ROW:
	case MACRO_ROW:
	case PARTITION_ROW:
	case SORT_ROW:
		return true;
	default:
		return false;
	}
}

// the shape of every row: the kind, five ids, begin/end snapshot, one uuid, six strings, two flags
const char *const CATALOG_COLUMNS =
    "'k': 'BIGINT', 'i1': 'BIGINT', 'i2': 'BIGINT', 'i3': 'BIGINT', 'i4': 'BIGINT', 'i5': 'BIGINT', 'bs': 'BIGINT', "
    "'es': 'BIGINT', 'u': 'UUID', 's1': 'VARCHAR', 's2': 'VARCHAR', 's3': 'VARCHAR', 's4': 'VARCHAR', 's5': 'VARCHAR', "
    "'s6': 'VARCHAR', 'f1': 'BOOLEAN', 'f2': 'BOOLEAN'";
const vector<string> SLOTS = {"i1", "i2", "i3", "i4", "i5", "bs", "es", "u",
                              "s1", "s2", "s3", "s4", "s5", "s6", "f1", "f2"};

//! One row of the catalog, copied out of its chunk.
struct CatalogRow {
	int64_t kind = 0;
	int64_t ids[5] = {0, 0, 0, 0, 0};
	bool id_null[5] = {true, true, true, true, true};
	int64_t begin = 0;
	bool begin_null = true;
	int64_t end = 0;
	bool end_null = true;
	string uuid;
	string strings[6];
	bool string_null[6] = {true, true, true, true, true, true};
	bool flags[2] = {false, false};

	uint64_t Id(idx_t i) const {
		return static_cast<uint64_t>(ids[i]);
	}
	bool VisibleAt(idx_t snapshot) const {
		if (!IsVersioned(kind)) {
			return true;
		}
		auto s = static_cast<int64_t>(snapshot);
		return !begin_null && begin <= s && (end_null || s < end);
	}
	//! what makes this row this row: its key columns and, for a version, the snapshot it began at
	string Key() const {
		string key = to_string(kind);
		idx_t key_ids = 0;
		switch (kind) {
		case COLUMN_ROW:
		case COLUMN_TAG_ROW:
		case INLINED_TABLE_ROW:
		case MACRO_IMPL_ROW:
		case PARTITION_COLUMN_ROW:
		case SORT_EXPRESSION_ROW:
			key_ids = 2;
			break;
		case MACRO_PARAMETER_ROW:
			key_ids = 3;
			break;
		default:
			key_ids = 1;
		}
		for (idx_t i = 0; i < key_ids; i++) {
			key += "|" + to_string(ids[i]);
		}
		if (IsVersioned(kind)) {
			key += "|" + to_string(begin);
		}
		if (kind == TAG_ROW || kind == COLUMN_TAG_ROW) {
			key += "|" + strings[0];
		} else if (kind == VIEW_COLUMN_TAG_ROW) {
			key += "|" + strings[0] + "|" + strings[1];
		}
		return key;
	}
};

CatalogRow ReadRow(DataChunk &chunk, idx_t row) {
	CatalogRow out;
	auto number = [&](idx_t column, int64_t &value, bool &is_null) {
		is_null = FlatVector::IsNull(chunk.data[column], row);
		value = is_null ? 0 : FlatVector::GetData<int64_t>(chunk.data[column])[row];
	};
	bool kind_null;
	number(0, out.kind, kind_null);
	for (idx_t i = 0; i < 5; i++) {
		number(1 + i, out.ids[i], out.id_null[i]);
	}
	number(6, out.begin, out.begin_null);
	number(7, out.end, out.end_null);
	if (!FlatVector::IsNull(chunk.data[8], row)) {
		out.uuid = chunk.GetValue(8, row).ToString();
	}
	for (idx_t i = 0; i < 6; i++) {
		out.string_null[i] = FlatVector::IsNull(chunk.data[9 + i], row);
		if (!out.string_null[i]) {
			out.strings[i] = FlatVector::GetData<string_t>(chunk.data[9 + i])[row].GetString();
		}
	}
	for (idx_t i = 0; i < 2; i++) {
		out.flags[i] =
		    !FlatVector::IsNull(chunk.data[15 + i], row) && FlatVector::GetData<bool>(chunk.data[15 + i])[row];
	}
	return out;
}

//! What the statement reads: the catalog at one snapshot (phase A), every row of its history that
//! a live snapshot can still see (the mirror's first load), what changed after a snapshot, or only
//! whether the inlined data tables moved - they change without a snapshot, so a mirror that answers
//! from what it holds asks that much first.
enum class CatalogRead { AT_SNAPSHOT, HISTORY, CHANGES, INLINED };

struct BranchBuilder {
	const string &schema;
	CatalogRead read;
	vector<string> branches;

	string Table(const string &name) const {
		return schema + "." + name;
	}
	//! the rows of a versioned table this read wants
	string Versioned(const string &alias) const {
		switch (read) {
		case CatalogRead::AT_SNAPSHOT:
			return StringUtil::Format("%s.begin_snapshot <= @s AND (@s < %s.end_snapshot OR %s.end_snapshot IS NULL)",
			                          alias, alias, alias);
		case CatalogRead::HISTORY:
			// a row that ended at or before the oldest snapshot is invisible to every snapshot left
			return StringUtil::Format("(%s.end_snapshot IS NULL OR %s.end_snapshot > @oldest)", alias, alias);
		default:
			return StringUtil::Format("(%s.begin_snapshot > @a OR %s.end_snapshot > @a)", alias, alias);
		}
	}
	//! the children of the parents this read wants: a new parent's children came with it
	string Children(const string &parent_table, const string &key, const string &alias) const {
		auto parent = read == CatalogRead::CHANGES ? string("p.begin_snapshot > @a") : Versioned("p");
		return StringUtil::Format("EXISTS (SELECT 1 FROM %s p WHERE p.%s = %s.%s AND %s)", Table(parent_table), key,
		                          alias, key, parent);
	}
	//! one branch: the given slots filled, the rest typed NULLs
	void Add(int64_t kind, const map<string, string> &slots, const string &from) {
		string select = StringUtil::Format("CAST(%lld AS BIGINT)", static_cast<long long>(kind));
		for (auto &slot : SLOTS) {
			auto entry = slots.find(slot);
			if (entry != slots.end()) {
				select += ", " + entry->second;
			} else if (slot[0] == 'u') {
				select += ", CAST(NULL AS UNIQUEIDENTIFIER)";
			} else if (slot[0] == 's') {
				// VARCHAR(1): a UNION column takes its widest branch, and MAX made every string MAX
				select += ", CAST(NULL AS VARCHAR(1))";
			} else if (slot[0] == 'f') {
				select += ", CAST(NULL AS BIT)";
			} else {
				select += ", CAST(NULL AS BIGINT)";
			}
		}
		branches.push_back("SELECT " + select + " FROM " + from);
	}
	void AddVersioned(int64_t kind, map<string, string> slots, const string &table, const string &alias) {
		slots["bs"] = alias + ".begin_snapshot";
		slots["es"] = alias + ".end_snapshot";
		Add(kind, slots, Table(table) + " " + alias + " WHERE " + Versioned(alias));
	}
};

//! The mirror's reads start with the snapshots, in a statement of their own and before anything
//! else: once snapshot @latest is seen committed, every snapshot up to it is (a commit takes the
//! next id under the primary key, so none overtakes another). Whatever a branch read after it sees
//! past it is a committed fact too, read again next time - never one missed.
string MirrorPreamble(const BranchBuilder &b) {
	return StringUtil::Format(
	    "DECLARE @latest BIGINT = (SELECT MAX(snapshot_id) FROM %s);\n"
	    "DECLARE @oldest BIGINT = (SELECT MIN(snapshot_id) FROM %s);\n"
	    "DECLARE @n BIGINT, @c BIGINT;\n"
	    "SELECT @n = COUNT_BIG(*), @c = ISNULL(CHECKSUM_AGG(CHECKSUM(table_id, schema_version, table_name)), 0) "
	    "FROM %s;\n",
	    b.Table("ducklake_snapshot"), b.Table("ducklake_snapshot"), b.Table("ducklake_inlined_data_tables"));
}

void AddMeta(BranchBuilder &b) {
	b.Add(META_ROW, {{"i1", "@latest"}, {"i2", "@oldest"}, {"i3", "@n"}, {"i4", "@c"}}, "(SELECT 1 AS one) meta");
}

string CatalogStatement(const string &schema, bool v1_1, CatalogRead read) {
	BranchBuilder b {schema, read, {}};
	if (read == CatalogRead::INLINED) {
		b.Add(INLINED_TABLE_ROW, {{"i1", "idt.table_id"}, {"i2", "idt.schema_version"}, {"s1", "idt.table_name"}},
		      b.Table("ducklake_inlined_data_tables") + " idt WHERE @n <> @known_n OR @c <> @known_c");
		AddMeta(b);
		return MirrorPreamble(b) + StringUtil::Join(b.branches, "\nUNION ALL\n");
	}
	map<string, string> schema_slots = {{"i1", "sch.schema_id"},
	                                    {"u", "sch.schema_uuid"},
	                                    {"s1", "sch.schema_name"},
	                                    {"s2", "sch.path"},
	                                    {"f1", "sch.path_is_relative"}};
	if (v1_1) {
		schema_slots["i2"] = "sch.parent_schema_id";
	}
	b.AddVersioned(SCHEMA_ROW, schema_slots, "ducklake_schema", "sch");
	b.AddVersioned(TABLE_ROW,
	               {{"i1", "tbl.table_id"},
	                {"i2", "tbl.schema_id"},
	                {"u", "tbl.table_uuid"},
	                {"s1", "tbl.table_name"},
	                {"s2", "tbl.path"},
	                {"f1", "tbl.path_is_relative"}},
	               "ducklake_table", "tbl");
	b.AddVersioned(COLUMN_ROW,
	               {{"i1", "col.table_id"},
	                {"i2", "col.column_id"},
	                {"i3", "col.column_order"},
	                {"i4", "col.parent_column"},
	                {"s1", "col.column_name"},
	                {"s2", "col.column_type"},
	                {"s3", "col.initial_default"},
	                {"s4", "col.default_value"},
	                {"s5", "col.default_value_type"},
	                {"f1", "col.nulls_allowed"}},
	               "ducklake_column", "col");
	b.AddVersioned(TAG_ROW, {{"i1", "tag.object_id"}, {"s1", "tag.[key]"}, {"s2", "tag.[value]"}}, "ducklake_tag",
	               "tag");
	b.AddVersioned(COLUMN_TAG_ROW,
	               {{"i1", "ct.table_id"}, {"i2", "ct.column_id"}, {"s1", "ct.[key]"}, {"s2", "ct.[value]"}},
	               "ducklake_column_tag", "ct");
	// every inlined data table, with no snapshot to filter by - as the base reads them. They change
	// without a snapshot (an empty one is dropped after a commit), so a read of changes brings them
	// all whenever their fingerprint moved, and none otherwise
	b.Add(INLINED_TABLE_ROW, {{"i1", "idt.table_id"}, {"i2", "idt.schema_version"}, {"s1", "idt.table_name"}},
	      b.Table("ducklake_inlined_data_tables") + " idt" +
	          (read == CatalogRead::CHANGES ? " WHERE @n <> @known_n OR @c <> @known_c" : ""));

	b.AddVersioned(VIEW_ROW,
	               {{"i1", "vw.view_id"},
	                {"i2", "vw.schema_id"},
	                {"u", "vw.view_uuid"},
	                {"s1", "vw.view_name"},
	                {"s2", "vw.dialect"},
	                {"s3", "vw.[sql]"},
	                {"s4", "vw.column_aliases"}},
	               "ducklake_view", "vw");
	if (v1_1) {
		b.AddVersioned(VIEW_COLUMN_TAG_ROW,
		               {{"i1", "vct.view_id"}, {"s1", "vct.column_name"}, {"s2", "vct.[key]"}, {"s3", "vct.[value]"}},
		               "ducklake_view_column_tag", "vct");
	}
	b.AddVersioned(MACRO_ROW, {{"i1", "mac.macro_id"}, {"i2", "mac.schema_id"}, {"s1", "mac.macro_name"}},
	               "ducklake_macro", "mac");
	b.Add(MACRO_IMPL_ROW,
	      {{"i1", "imp.macro_id"},
	       {"i2", "imp.impl_id"},
	       {"s1", "imp.dialect"},
	       {"s2", "imp.[sql]"},
	       {"s3", "imp.[type]"}},
	      b.Table("ducklake_macro_impl") + " imp WHERE " + b.Children("ducklake_macro", "macro_id", "imp"));
	b.Add(MACRO_PARAMETER_ROW,
	      {{"i1", "par.macro_id"},
	       {"i2", "par.impl_id"},
	       {"i3", "par.column_id"},
	       {"s1", "par.parameter_name"},
	       {"s2", "par.parameter_type"},
	       {"s3", "par.default_value"},
	       {"s4", "par.default_value_type"}},
	      b.Table("ducklake_macro_parameters") + " par WHERE " + b.Children("ducklake_macro", "macro_id", "par"));
	b.AddVersioned(PARTITION_ROW, {{"i1", "part.partition_id"}, {"i2", "part.table_id"}}, "ducklake_partition_info",
	               "part");
	b.Add(PARTITION_COLUMN_ROW,
	      {{"i1", "pc.partition_id"}, {"i2", "pc.partition_key_index"}, {"i3", "pc.column_id"}, {"s1", "pc.transform"}},
	      b.Table("ducklake_partition_column") + " pc WHERE " +
	          b.Children("ducklake_partition_info", "partition_id", "pc"));
	b.AddVersioned(SORT_ROW, {{"i1", "srt.sort_id"}, {"i2", "srt.table_id"}}, "ducklake_sort_info", "srt");
	b.Add(SORT_EXPRESSION_ROW,
	      {{"i1", "se.sort_id"},
	       {"i2", "se.sort_key_index"},
	       {"s1", "se.expression"},
	       {"s2", "se.dialect"},
	       {"s3", "se.sort_direction"},
	       {"s4", "se.null_order"}},
	      b.Table("ducklake_sort_expression") + " se WHERE " + b.Children("ducklake_sort_info", "sort_id", "se"));
	if (read == CatalogRead::AT_SNAPSHOT) {
		return StringUtil::Join(b.branches, "\nUNION ALL\n");
	}
	AddMeta(b);
	return MirrorPreamble(b) + StringUtil::Join(b.branches, "\nUNION ALL\n");
}

//! the base's AddChildColumn: a nested column goes under its parent, wherever that sits
bool AddChildColumn(vector<DuckLakeColumnInfo> &columns, FieldIndex parent_id, DuckLakeColumnInfo &column_info) {
	for (auto &col : columns) {
		if (col.id == parent_id) {
			col.children.push_back(std::move(column_info));
			return true;
		}
		if (AddChildColumn(col.children, parent_id, column_info)) {
			return true;
		}
	}
	return false;
}

Value StringValue(const CatalogRow &row, idx_t i) {
	return row.string_null[i] ? Value() : Value(row.strings[i]);
}

//! The catalog at `snapshot` from catalog rows, the way BuildCatalogForSnapshot builds it from its
//! seven queries: the same fields, the same order where the base orders, the same refusals.
DuckLakeCatalogInfo AssembleCatalog(const vector<reference<const CatalogRow>> &all_rows, idx_t snapshot,
                                    const string &base_data_path, const string &separator, bool v1_1) {
	map<int64_t, vector<reference<const CatalogRow>>> rows;
	for (auto &row : all_rows) {
		if (row.get().VisibleAt(snapshot)) {
			rows[row.get().kind].push_back(row);
		}
	}
	auto sorted = [&](int64_t kind, std::function<bool(const CatalogRow &, const CatalogRow &)> less) {
		auto &list = rows[kind];
		std::stable_sort(list.begin(), list.end(),
		                 [&](reference<const CatalogRow> a, reference<const CatalogRow> b) { return less(a, b); });
		return list;
	};
	auto by_id = [](const CatalogRow &a, const CatalogRow &b) {
		return a.ids[0] < b.ids[0];
	};

	DuckLakeCatalogInfo catalog;
	map<idx_t, idx_t> schema_map;
	for (const CatalogRow &row : sorted(SCHEMA_ROW, by_id)) {
		DuckLakeSchemaInfo schema;
		schema.id = SchemaIndex(row.Id(0));
		schema.uuid = row.uuid;
		schema.name = row.strings[0];
		if (row.string_null[1]) {
			schema.path = base_data_path;
		} else {
			DuckLakePath path {row.strings[1], row.flags[0]};
			schema.path = DuckLakeMetadataManager::FromRelativePath(path, base_data_path, separator);
		}
		if (v1_1 && !row.id_null[1]) {
			schema.parent_id = SchemaIndex(row.Id(1));
		}
		schema_map[schema.id.index] = catalog.schemas.size();
		catalog.schemas.push_back(std::move(schema));
	}

	// tags by object, column tags by (table, column), inlined tables by table
	map<idx_t, vector<DuckLakeTag>> object_tags;
	for (const CatalogRow &row : rows[TAG_ROW]) {
		// the base's LoadTags: a tag without a value is skipped
		if (!row.string_null[1]) {
			object_tags[row.Id(0)].push_back({row.strings[0], row.strings[1]});
		}
	}
	map<pair<idx_t, idx_t>, vector<DuckLakeTag>> column_tags;
	for (const CatalogRow &row : rows[COLUMN_TAG_ROW]) {
		if (!row.string_null[1]) {
			column_tags[{row.Id(0), row.Id(1)}].push_back({row.strings[0], row.strings[1]});
		}
	}
	map<idx_t, vector<DuckLakeInlinedTableInfo>> inlined_tables;
	for (const CatalogRow &row :
	     sorted(INLINED_TABLE_ROW, [](const CatalogRow &a, const CatalogRow &b) { return a.ids[1] < b.ids[1]; })) {
		inlined_tables[row.Id(0)].push_back({row.strings[0], row.Id(1)});
	}

	// tables, with their columns in the base's order: parent_column NULLS FIRST, then column_order
	map<idx_t, vector<reference<const CatalogRow>>> table_columns;
	for (const CatalogRow &row : rows[COLUMN_ROW]) {
		table_columns[row.Id(0)].push_back(row);
	}
	std::function<void(vector<DuckLakeColumnInfo> &, idx_t)> attach_column_tags =
	    [&](vector<DuckLakeColumnInfo> &columns, idx_t table_id) {
		    for (auto &column : columns) {
			    auto entry = column_tags.find({table_id, column.id.index});
			    if (entry != column_tags.end()) {
				    column.tags = entry->second;
			    }
			    attach_column_tags(column.children, table_id);
		    }
	    };
	for (const CatalogRow &row : sorted(TABLE_ROW, by_id)) {
		DuckLakeTableInfo table;
		table.id = TableIndex(row.Id(0));
		table.schema_id = SchemaIndex(row.Id(1));
		table.uuid = row.uuid;
		table.name = row.strings[0];
		auto schema_entry = schema_map.find(table.schema_id.index);
		if (schema_entry == schema_map.end()) {
			throw InvalidInputException(
			    "Failed to load DuckLake - table with id %d references schema id %d that does not exist",
			    table.id.index, table.schema_id.index);
		}
		auto &schema = catalog.schemas[schema_entry->second];
		if (row.string_null[1]) {
			table.path = schema.path;
		} else {
			DuckLakePath path {row.strings[1], row.flags[0]};
			table.path = DuckLakeMetadataManager::FromRelativePath(path, schema.path, separator);
		}
		auto columns = table_columns.find(table.id.index);
		if (columns == table_columns.end() || columns->second.empty()) {
			throw InvalidInputException("Failed to load DuckLake - Table entry \"%s\" does not have any columns",
			                            table.name);
		}
		auto &list = columns->second;
		std::stable_sort(list.begin(), list.end(), [](reference<const CatalogRow> a, reference<const CatalogRow> b) {
			const CatalogRow &x = a, &y = b;
			if (x.id_null[3] != y.id_null[3]) {
				return x.id_null[3];
			}
			if (x.ids[3] != y.ids[3]) {
				return x.ids[3] < y.ids[3];
			}
			return x.ids[2] < y.ids[2];
		});
		for (const CatalogRow &col : list) {
			DuckLakeColumnInfo column;
			column.id = FieldIndex(col.Id(1));
			column.name = col.strings[0];
			column.type = col.strings[1];
			if (!col.string_null[2]) {
				column.initial_default = Value(col.strings[2]);
			}
			if (!col.string_null[3]) {
				column.default_value = col.strings[3] == "NULL" ? Value() : Value(col.strings[3]);
			}
			if (!col.string_null[4]) {
				column.default_value_type = col.strings[4];
			}
			column.nulls_allowed = col.flags[0];
			if (col.id_null[3]) {
				table.columns.push_back(std::move(column));
			} else if (!AddChildColumn(table.columns, FieldIndex(col.Id(3)), column)) {
				throw InvalidInputException("Failed to load DuckLake - Could not find parent column for column %s",
				                            column.name);
			}
		}
		attach_column_tags(table.columns, table.id.index);
		auto tags = object_tags.find(table.id.index);
		if (tags != object_tags.end()) {
			table.tags = tags->second;
		}
		auto inlined = inlined_tables.find(table.id.index);
		if (inlined != inlined_tables.end()) {
			table.inlined_data_tables = inlined->second;
		}
		catalog.tables.push_back(std::move(table));
	}

	map<idx_t, vector<DuckLakeViewColumnTag>> view_column_tags;
	for (const CatalogRow &row : rows[VIEW_COLUMN_TAG_ROW]) {
		DuckLakeViewColumnTag tag;
		tag.column_name = row.strings[0];
		tag.key = row.strings[1];
		tag.value = StringValue(row, 2);
		view_column_tags[row.Id(0)].push_back(std::move(tag));
	}
	for (const CatalogRow &row : sorted(VIEW_ROW, by_id)) {
		DuckLakeViewInfo view;
		view.id = TableIndex(row.Id(0));
		view.schema_id = SchemaIndex(row.Id(1));
		view.uuid = row.uuid;
		view.name = row.strings[0];
		view.dialect = row.strings[1];
		view.sql = row.strings[2];
		if (!row.string_null[3]) {
			view.column_aliases = DuckLakeUtil::ParseQuotedList(row.strings[3]);
		}
		auto tags = object_tags.find(view.id.index);
		if (tags != object_tags.end()) {
			view.tags = tags->second;
		}
		auto vct = view_column_tags.find(view.id.index);
		if (vct != view_column_tags.end()) {
			view.column_tags = vct->second;
		}
		catalog.views.push_back(std::move(view));
	}

	// macros: their implementations by impl_id, each implementation's parameters by column_id
	map<pair<idx_t, idx_t>, vector<DuckLakeMacroParameters>> parameters;
	for (const CatalogRow &row :
	     sorted(MACRO_PARAMETER_ROW, [](const CatalogRow &a, const CatalogRow &b) { return a.ids[2] < b.ids[2]; })) {
		DuckLakeMacroParameters parameter;
		parameter.parameter_name = row.strings[0];
		parameter.parameter_type = row.strings[1];
		parameter.default_value = StringValue(row, 2);
		parameter.default_value_type = row.string_null[3] ? string() : row.strings[3];
		parameters[{row.Id(0), row.Id(1)}].push_back(std::move(parameter));
	}
	map<idx_t, vector<DuckLakeMacroImplementation>> implementations;
	for (const CatalogRow &row :
	     sorted(MACRO_IMPL_ROW, [](const CatalogRow &a, const CatalogRow &b) { return a.ids[1] < b.ids[1]; })) {
		DuckLakeMacroImplementation impl;
		impl.dialect = row.strings[0];
		impl.sql = row.strings[1];
		impl.type = row.strings[2];
		auto entry = parameters.find({row.Id(0), row.Id(1)});
		if (entry != parameters.end()) {
			impl.parameters = entry->second;
		}
		implementations[row.Id(0)].push_back(std::move(impl));
	}
	for (const CatalogRow &row : sorted(MACRO_ROW, by_id)) {
		DuckLakeMacroInfo macro;
		macro.macro_id = MacroIndex(row.Id(0));
		macro.schema_id = SchemaIndex(row.Id(1));
		macro.macro_name = row.strings[0];
		auto entry = implementations.find(row.Id(0));
		if (entry != implementations.end()) {
			macro.implementations = entry->second;
		}
		catalog.macros.push_back(std::move(macro));
	}

	// partitions and sorts: ordered by table, then the spec's id, then the key - the base's ORDER BY -
	// and one entry per table, carrying its first spec's id
	auto keyed_children = [&](int64_t kind) {
		map<idx_t, vector<reference<const CatalogRow>>> out;
		for (const CatalogRow &row : rows[kind]) {
			out[row.Id(0)].push_back(row);
		}
		for (auto &entry : out) {
			std::stable_sort(entry.second.begin(), entry.second.end(),
			                 [](reference<const CatalogRow> a, reference<const CatalogRow> b) {
				                 return a.get().ids[1] < b.get().ids[1];
			                 });
		}
		return out;
	};
	auto by_table_then_id = [](const CatalogRow &a, const CatalogRow &b) {
		return a.ids[1] != b.ids[1] ? a.ids[1] < b.ids[1] : a.ids[0] < b.ids[0];
	};
	auto partition_columns = keyed_children(PARTITION_COLUMN_ROW);
	for (const CatalogRow &row : sorted(PARTITION_ROW, by_table_then_id)) {
		auto table_id = TableIndex(row.Id(1));
		auto fields = partition_columns.find(row.Id(0));
		if (fields == partition_columns.end()) {
			continue; // the base's inner join: a spec without columns is not loaded
		}
		if (catalog.partitions.empty() || catalog.partitions.back().table_id != table_id) {
			DuckLakePartitionInfo partition;
			partition.id = row.Id(0);
			partition.table_id = table_id;
			catalog.partitions.push_back(std::move(partition));
		}
		for (const CatalogRow &col : fields->second) {
			DuckLakePartitionFieldInfo field;
			field.partition_key_index = col.Id(1);
			field.field_id = FieldIndex(col.Id(2));
			field.transform = col.strings[0];
			catalog.partitions.back().fields.push_back(std::move(field));
		}
	}
	auto sort_expressions = keyed_children(SORT_EXPRESSION_ROW);
	for (const CatalogRow &row : sorted(SORT_ROW, by_table_then_id)) {
		auto table_id = TableIndex(row.Id(1));
		auto fields = sort_expressions.find(row.Id(0));
		if (fields == sort_expressions.end()) {
			continue;
		}
		if (catalog.sorts.empty() || catalog.sorts.back().table_id != table_id) {
			DuckLakeSortInfo sort;
			sort.id = row.Id(0);
			sort.table_id = table_id;
			catalog.sorts.push_back(std::move(sort));
		}
		for (const CatalogRow &expr : fields->second) {
			DuckLakeSortFieldInfo field;
			field.sort_key_index = expr.Id(1);
			field.expression = expr.strings[0];
			field.dialect = expr.strings[1];
			field.sort_direction =
			    StringUtil::CIEquals(expr.strings[2], "DESC") ? OrderType::DESCENDING : OrderType::ASCENDING;
			field.null_order = StringUtil::CIEquals(expr.strings[3], "NULLS_FIRST") ? OrderByNullType::NULLS_FIRST
			                                                                        : OrderByNullType::NULLS_LAST;
			catalog.sorts.back().fields.push_back(std::move(field));
		}
	}
	return catalog;
}

//! The mirror of one attached catalog (phase B), in the DuckDB instance's object cache under the
//! attached database's oid - one per ATTACH, never shared between lakes or two attaches of one.
struct MSSQLCatalogMirror : public ObjectCacheEntry {
	std::mutex lock;
	bool loaded = false;
	//! the catalog grew past the row budget: this attach loads at each snapshot instead (phase A)
	bool over_budget = false;
	//! the oldest snapshot the rows can answer for, and the one they are caught up to
	idx_t base = 0;
	idx_t as_of = 0;
	//! the fingerprint of ducklake_inlined_data_tables the rows were read with
	int64_t inlined_count = -1;
	int64_t inlined_checksum = 0;
	vector<unique_ptr<CatalogRow>> rows;
	std::unordered_map<string, idx_t> index;

	static string ObjectType() {
		return "mssql_ducklake_catalog_mirror";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx(); // never evicted: losing it costs a full load, keeping it a few MB
	}

	//! Merge rows read from the server: a new row is added; a known one can only have ended since,
	//! so its end_snapshot is taken if it was open. Reading a row twice, or two readers' rows in
	//! either order, ends in the same state.
	void Merge(vector<CatalogRow> &read, bool inlined_changed) {
		if (inlined_changed) {
			for (auto &row : rows) {
				if (row && row->kind == INLINED_TABLE_ROW) {
					index.erase(row->Key());
					row.reset();
				}
			}
		}
		for (auto &row : read) {
			auto key = row.Key();
			auto entry = index.find(key);
			if (entry == index.end()) {
				index[key] = rows.size();
				rows.push_back(make_uniq<CatalogRow>(std::move(row)));
			} else if (!row.end_null && rows[entry->second]->end_null) {
				rows[entry->second]->end = row.end;
				rows[entry->second]->end_null = false;
			}
		}
	}

	//! Drop the versions no snapshot left can see - expire_snapshots removed their snapshots.
	void Prune(idx_t oldest) {
		bool dropped = false;
		for (auto &row : rows) {
			if (row && IsVersioned(row->kind) && !row->end_null && row->end <= static_cast<int64_t>(oldest)) {
				row.reset();
				dropped = true;
			}
		}
		if (dropped || rows.size() > 2 * index.size()) {
			vector<unique_ptr<CatalogRow>> kept;
			index.clear();
			for (auto &row : rows) {
				if (row) {
					index[row->Key()] = kept.size();
					kept.push_back(std::move(row));
				}
			}
			rows = std::move(kept);
		}
	}

	vector<reference<const CatalogRow>> Rows() const {
		vector<reference<const CatalogRow>> out;
		out.reserve(rows.size());
		for (auto &row : rows) {
			if (row) {
				out.push_back(*row);
			}
		}
		return out;
	}
};

vector<CatalogRow> ReadCatalogRows(QueryResult &result) {
	vector<CatalogRow> rows;
	while (true) {
		auto chunk = result.Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		chunk->Flatten();
		for (idx_t row = 0; row < chunk->size(); row++) {
			rows.push_back(ReadRow(*chunk, row));
		}
	}
	return rows;
}

} // namespace

DuckLakeCatalogInfo MSSQLMetadataManager::GetCatalogForSnapshot(DuckLakeSnapshot snapshot) {
	if (CatalogStatementDisabled()) {
		return DuckLakeMetadataManager::GetCatalogForSnapshot(snapshot);
	}
	auto catalog = LoadCatalog(snapshot);
	if (VerifyCatalogEnabled()) {
		auto base = DuckLakeMetadataManager::GetCatalogForSnapshot(snapshot);
		auto difference = CatalogDifference(base, catalog);
		if (!difference.empty()) {
			throw InvalidInputException("mssql_ducklake: the catalog load differs from DuckLake's at snapshot %llu: %s",
			                            snapshot.snapshot_id, difference);
		}
	}
	return catalog;
}

DuckLakeCatalogInfo MSSQLMetadataManager::LoadCatalog(DuckLakeSnapshot snapshot) {
	auto &ducklake_catalog = transaction.GetCatalog();
	const bool v1_1 = ducklake_catalog.SupportsV1_1Metadata();
	const auto &base_data_path = ducklake_catalog.DataPath();
	const auto &separator = ducklake_catalog.Separator();
	const auto schema = SchemaIdentifier();

	auto read = [&](CatalogRead what, const string &params, const string &declarations) {
		auto query = StringUtil::Format(
		    "SELECT * FROM mssql_scan_params_unsafe({METADATA_CATALOG_NAME_LITERAL}, %s, {%s}, %s, columns := {%s})",
		    SQLString(CatalogStatement(schema, v1_1, what)), params, SQLString(declarations), CATALOG_COLUMNS);
		auto result = Query(snapshot, query);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to load the DuckLake catalog from SQL Server: ");
		}
		return ReadCatalogRows(*result);
	};
	auto at_snapshot = [&]() {
		auto rows = read(CatalogRead::AT_SNAPSHOT, "'s': {SNAPSHOT_ID}", "@s BIGINT");
		vector<reference<const CatalogRow>> refs(rows.begin(), rows.end());
		return AssembleCatalog(refs, snapshot.snapshot_id, base_data_path, separator, v1_1);
	};

	// A transaction that has written reads its own uncommitted rows on its connection: those are not
	// the committed history the mirror holds, so it reads at its snapshot and leaves the mirror be.
	auto client_context = transaction.context.lock();
	if (CatalogMirrorDisabled() || wrote_in_transaction || !client_context) {
		return at_snapshot();
	}
	auto &cache = ObjectCache::GetObjectCache(*client_context);
	auto key = StringUtil::Format("mssql_ducklake:catalog_mirror:%llu", ducklake_catalog.GetAttached().oid);
	auto mirror = cache.GetOrCreate<MSSQLCatalogMirror>(key);

	bool full;
	bool held;
	idx_t as_of;
	int64_t known_count, known_checksum;
	{
		std::lock_guard<std::mutex> guard(mirror->lock);
		if (mirror->over_budget) {
			return at_snapshot();
		}
		// held: the rows of every snapshot up to as_of. A flush or a compaction asks for older schema
		// versions, one per table - only whether the inlined tables moved is read for those
		held = mirror->loaded && snapshot.snapshot_id >= mirror->base && snapshot.snapshot_id <= mirror->as_of;
		full = !mirror->loaded;
		as_of = mirror->as_of;
		known_count = mirror->inlined_count;
		known_checksum = mirror->inlined_checksum;
	}

	// the round trip runs without the lock: a reader waiting on the server never holds up the rest
	vector<CatalogRow> rows;
	if (full) {
		rows = read(CatalogRead::HISTORY, "", "");
	} else {
		rows = read(held ? CatalogRead::INLINED : CatalogRead::CHANGES,
		            StringUtil::Format("'a': %llu, 'known_n': %lld, 'known_c': %lld", as_of,
		                               static_cast<long long>(known_count), static_cast<long long>(known_checksum)),
		            "@a BIGINT, @known_n BIGINT, @known_c BIGINT");
	}
	auto meta = std::find_if(rows.begin(), rows.end(), [](const CatalogRow &row) { return row.kind == META_ROW; });
	if (meta == rows.end() || meta->id_null[0]) {
		// an empty snapshot table: nothing to mirror yet
		return at_snapshot();
	}
	const auto latest = static_cast<idx_t>(meta->ids[0]);
	const auto oldest = static_cast<idx_t>(meta->ids[1]);
	const auto count = meta->ids[2];
	const auto checksum = meta->ids[3];
	rows.erase(meta);

	std::lock_guard<std::mutex> guard(mirror->lock);
	if (mirror->over_budget) {
		return at_snapshot();
	}
	const bool inlined_changed = count != mirror->inlined_count || checksum != mirror->inlined_checksum;
	// a full read brings every inlined table, a read of changes or of the fingerprint only when it moved
	mirror->Merge(rows, full || inlined_changed);
	if (inlined_changed) {
		mirror->inlined_count = count;
		mirror->inlined_checksum = checksum;
	}
	if (!mirror->loaded) {
		mirror->loaded = true;
		mirror->base = oldest;
	}
	// only a read of rows moves as_of: the fingerprint read of a held snapshot looked at no versioned
	// table, so a snapshot committed since is still to be read
	if (!held) {
		mirror->as_of = MaxValue(mirror->as_of, latest);
	}
	if (oldest > mirror->base) {
		mirror->Prune(oldest);
		mirror->base = oldest;
	}
	if (mirror->rows.size() > CatalogMirrorRowBudget()) {
		mirror->over_budget = true;
		mirror->rows.clear();
		mirror->index.clear();
		return at_snapshot();
	}
	if (snapshot.snapshot_id >= mirror->base && snapshot.snapshot_id <= mirror->as_of) {
		return AssembleCatalog(mirror->Rows(), snapshot.snapshot_id, base_data_path, separator, v1_1);
	}
	// a snapshot past what the server just said is the latest, or before the oldest: not ours to answer
	return at_snapshot();
}

// The verify switch (specs/020 B6): the base's answer and ours, field by field. Lists the base
// builds with LIST() carry no order (tags, inlined tables, view column tags), so those are compared
// sorted; everything else in the order it came.
namespace {

template <class T, class LESS>
vector<T> Sorted(vector<T> items, LESS less) {
	std::sort(items.begin(), items.end(), less);
	return items;
}

string TagsText(const vector<DuckLakeTag> &tags) {
	auto sorted = Sorted(tags, [](const DuckLakeTag &a, const DuckLakeTag &b) {
		return a.key != b.key ? a.key < b.key : a.value < b.value;
	});
	string out;
	for (auto &tag : sorted) {
		out += tag.key + "=" + tag.value + ";";
	}
	return out;
}

string ColumnText(const DuckLakeColumnInfo &column) {
	string out = StringUtil::Format("%llu:%s:%s:%s:%s:%s:%d:[%s]", column.id.index, column.name, column.type,
	                                column.initial_default.ToSQLString(), column.default_value.ToSQLString(),
	                                column.default_value_type, column.nulls_allowed, TagsText(column.tags));
	out += "{";
	for (auto &child : column.children) {
		out += ColumnText(child) + ",";
	}
	return out + "}";
}

string TableText(const DuckLakeTableInfo &table) {
	auto inlined =
	    Sorted(table.inlined_data_tables, [](const DuckLakeInlinedTableInfo &a, const DuckLakeInlinedTableInfo &b) {
		    return a.schema_version != b.schema_version ? a.schema_version < b.schema_version
		                                                : a.table_name < b.table_name;
	    });
	string out = StringUtil::Format("table %llu schema %llu %s %s %s tags[%s] inlined[", table.id.index,
	                                table.schema_id.index, table.uuid, table.name, table.path, TagsText(table.tags));
	for (auto &entry : inlined) {
		out += StringUtil::Format("%s@%llu,", entry.table_name, entry.schema_version);
	}
	out += "] columns[";
	for (auto &column : table.columns) {
		out += ColumnText(column) + ",";
	}
	return out + "]";
}

string ViewText(const DuckLakeViewInfo &view) {
	auto vct = Sorted(view.column_tags, [](const DuckLakeViewColumnTag &a, const DuckLakeViewColumnTag &b) {
		return a.column_name != b.column_name ? a.column_name < b.column_name : a.key < b.key;
	});
	string out = StringUtil::Format("view %llu schema %llu %s %s %s %s aliases[%s] tags[%s] column_tags[",
	                                view.id.index, view.schema_id.index, view.uuid, view.name, view.dialect, view.sql,
	                                StringUtil::Join(view.column_aliases, ","), TagsText(view.tags));
	for (auto &tag : vct) {
		out += tag.column_name + "." + tag.key + "=" + tag.value.ToSQLString() + ";";
	}
	return out + "]";
}

string MacroText(const DuckLakeMacroInfo &macro) {
	string out =
	    StringUtil::Format("macro %llu schema %llu %s", macro.macro_id.index, macro.schema_id.index, macro.macro_name);
	for (auto &impl : macro.implementations) {
		out += StringUtil::Format(" impl(%s,%s,%s", impl.dialect, impl.sql, impl.type);
		for (auto &parameter : impl.parameters) {
			out += StringUtil::Format(",%s:%s:%s:%s", parameter.parameter_name, parameter.parameter_type,
			                          parameter.default_value.ToSQLString(), parameter.default_value_type);
		}
		out += ")";
	}
	return out;
}

template <class T, class TEXT>
string ListDifference(const string &what, const vector<T> &base, const vector<T> &ours, TEXT text) {
	for (idx_t i = 0; i < MaxValue(base.size(), ours.size()); i++) {
		auto a = i < base.size() ? text(base[i]) : string("<missing>");
		auto b = i < ours.size() ? text(ours[i]) : string("<missing>");
		if (a != b) {
			return StringUtil::Format("%s #%llu: DuckLake's %s, ours %s", what, i, a, b);
		}
	}
	return string();
}

} // namespace

string MSSQLMetadataManager::CatalogDifference(const DuckLakeCatalogInfo &base, const DuckLakeCatalogInfo &ours) {
	// the base reads schemas, views and macros in no order; ours come by id
	auto by_schema = [](const DuckLakeSchemaInfo &a, const DuckLakeSchemaInfo &b) {
		return a.id.index < b.id.index;
	};
	auto by_view = [](const DuckLakeViewInfo &a, const DuckLakeViewInfo &b) {
		return a.id.index < b.id.index;
	};
	auto by_macro = [](const DuckLakeMacroInfo &a, const DuckLakeMacroInfo &b) {
		return a.macro_id.index < b.macro_id.index;
	};
	auto difference =
	    ListDifference("schema", Sorted(base.schemas, by_schema), Sorted(ours.schemas, by_schema),
	                   [](const DuckLakeSchemaInfo &schema) {
		                   return StringUtil::Format("%llu %llu %s %s %s", schema.id.index, schema.parent_id.index,
		                                             schema.uuid, schema.name, schema.path);
	                   });
	if (difference.empty()) {
		difference = ListDifference("table", base.tables, ours.tables, TableText);
	}
	if (difference.empty()) {
		difference = ListDifference("view", Sorted(base.views, by_view), Sorted(ours.views, by_view), ViewText);
	}
	if (difference.empty()) {
		difference = ListDifference("macro", Sorted(base.macros, by_macro), Sorted(ours.macros, by_macro), MacroText);
	}
	if (difference.empty()) {
		difference = ListDifference("partition", base.partitions, ours.partitions, [](const DuckLakePartitionInfo &p) {
			string out = StringUtil::Format("%s table %llu", p.id.IsValid() ? to_string(p.id.GetIndex()) : "-",
			                                p.table_id.index);
			for (auto &field : p.fields) {
				out += StringUtil::Format(" %llu:%llu:%s", field.partition_key_index, field.field_id.index,
				                          field.transform);
			}
			return out;
		});
	}
	if (difference.empty()) {
		difference = ListDifference("sort", base.sorts, ours.sorts, [](const DuckLakeSortInfo &s) {
			string out = StringUtil::Format("%s table %llu", s.id.IsValid() ? to_string(s.id.GetIndex()) : "-",
			                                s.table_id.index);
			for (auto &field : s.fields) {
				out += StringUtil::Format(" %llu:%s:%s:%d:%d", field.sort_key_index, field.expression, field.dialect,
				                          static_cast<int>(field.sort_direction), static_cast<int>(field.null_order));
			}
			return out;
		});
	}
	return difference;
}

} // namespace duckdb
