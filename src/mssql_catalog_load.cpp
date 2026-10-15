#include "mssql_metadata_manager.hpp"
#include "mssql_metadata_internal.hpp"

#include <algorithm>

#include "common/ducklake_util.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

// The catalog load (specs/020). DuckLake rebuilds its catalog at a snapshot from fifteen tables in
// seven queries (BuildCatalogForSnapshot); through the catalog path every one of them is several
// scans joined on the client, each a round trip. Here it is one statement the server answers
// alone: a UNION ALL of one branch per kind of row, tagged by `k`, in one fixed shape. DuckDB orders
// the rows by `k` and the order keys o1-o4, which encode the base's own ORDER BYs, so they are
// built in the order the base builds them. Nothing is cached - every load is a
// fresh read of committed rows, which is why another node writing the same catalog cannot make it
// wrong.

namespace {

enum CatalogRowKind : int64_t {
	SCHEMA_ROW = 1,
	TABLE_ROW = 2,
	TAG_ROW = 3,
	COLUMN_TAG_ROW = 4,
	INLINED_TABLE_ROW = 5,
	VIEW_ROW = 6,
	VIEW_COLUMN_TAG_ROW = 7,
	MACRO_ROW = 8,
	MACRO_IMPL_ROW = 9,
	MACRO_PARAMETER_ROW = 10,
	PARTITION_ROW = 11,
	SORT_ROW = 12
};

// the columns of every row: k, the order keys o1-o4, ids i1-i4, one uuid, strings s1-s7, flags b1-b2
const char *const CATALOG_COLUMNS =
    "'k': 'BIGINT', 'o1': 'BIGINT', 'o2': 'BIGINT', 'o3': 'BIGINT', 'o4': 'BIGINT', 'i1': 'BIGINT', 'i2': 'BIGINT', "
    "'i3': 'BIGINT', 'i4': 'BIGINT', 'u': 'UUID', 's1': 'VARCHAR', 's2': 'VARCHAR', 's3': 'VARCHAR', 's4': 'VARCHAR', "
    "'s5': 'VARCHAR', 's6': 'VARCHAR', 's7': 'VARCHAR', 'b1': 'BOOLEAN', 'b2': 'BOOLEAN'";
enum CatalogColumn : idx_t { K, O1, O2, O3, O4, I1, I2, I3, I4, U, S1, S2, S3, S4, S5, S6, S7, B1, B2 };

//! One branch: `select` lists the nineteen columns in order, NULL where the kind has nothing.
string Branch(const string &select, const string &from) {
	return "SELECT " + select + " FROM " + from;
}

string Visible(const string &alias) {
	return StringUtil::Format("%s.begin_snapshot <= @s AND (@s < %s.end_snapshot OR %s.end_snapshot IS NULL)", alias,
	                          alias, alias);
}

//! The statement. `uuid`, strings and flags are typed per column so the branches agree without a
//! conversion the server would have to guess; the strings are the catalog's own columns, which the
//! shaping gave one collation (specs/006), so the UNION never has two to reconcile. The NULL
//! placeholders are VARCHAR(1): a column of the UNION takes the widest branch, and a MAX placeholder
//! made every string column MAX - 26 ms of server CPU a load for 300 tables, against 14.
string CatalogStatement(const string &schema, bool v1_1) {
	const string n = "CAST(NULL AS BIGINT)";
	const string nu = "CAST(NULL AS UNIQUEIDENTIFIER)";
	const string ns = "CAST(NULL AS VARCHAR(1))";
	const string nb = "CAST(NULL AS BIT)";
	auto t = [&](const string &name) {
		return schema + "." + name;
	};
	vector<string> branches;
	// schemas, by id
	branches.push_back(
	    Branch(StringUtil::Format("CAST(%d AS BIGINT), sch.schema_id, 0, 0, 0, sch.schema_id, %s, %s, %s, "
	                              "sch.schema_uuid, sch.schema_name, sch.path, %s, %s, %s, %s, %s, "
	                              "sch.path_is_relative, %s",
	                              SCHEMA_ROW, v1_1 ? "sch.parent_schema_id" : n, n, n, ns, ns, ns, ns, ns, nb),
	           t("ducklake_schema") + " sch WHERE " + Visible("sch")));
	// tables with their visible columns: table_id, parent_column NULLS FIRST, column_order - the base's
	// ORDER BY. The column filter sits in the join (as the load rewrite of specs/015 puts it): a table
	// keeps its row when it has no visible column, and the client refuses it as the base does.
	branches.push_back(Branch(
	    StringUtil::Format("CAST(%d AS BIGINT), tbl.table_id, CASE WHEN col.parent_column IS NULL THEN 0 ELSE 1 END, "
	                       "ISNULL(col.parent_column, 0), ISNULL(col.column_order, 0), tbl.table_id, tbl.schema_id, "
	                       "col.column_id, col.parent_column, tbl.table_uuid, tbl.table_name, tbl.path, "
	                       "col.column_name, col.column_type, col.initial_default, col.default_value, "
	                       "col.default_value_type, tbl.path_is_relative, col.nulls_allowed",
	                       TABLE_ROW),
	    t("ducklake_table") + " tbl LEFT JOIN " + t("ducklake_column") + " col ON col.table_id = tbl.table_id AND " +
	        Visible("col") + " WHERE " + Visible("tbl")));
	// the tags of tables and views alike: object ids are catalog ids, shared by both
	branches.push_back(
	    Branch(StringUtil::Format("CAST(%d AS BIGINT), tag.object_id, 0, 0, 0, tag.object_id, %s, %s, %s, %s, "
	                              "tag.[key], tag.[value], %s, %s, %s, %s, %s, %s, %s",
	                              TAG_ROW, n, n, n, nu, ns, ns, ns, ns, ns, nb, nb),
	           t("ducklake_tag") + " tag WHERE " + Visible("tag")));
	branches.push_back(
	    Branch(StringUtil::Format("CAST(%d AS BIGINT), ct.table_id, ct.column_id, 0, 0, ct.table_id, ct.column_id, "
	                              "%s, %s, %s, ct.[key], ct.[value], %s, %s, %s, %s, %s, %s, %s",
	                              COLUMN_TAG_ROW, n, n, nu, ns, ns, ns, ns, ns, nb, nb),
	           t("ducklake_column_tag") + " ct WHERE " + Visible("ct")));
	// every inlined data table, with no snapshot filter - as the base reads them
	branches.push_back(
	    Branch(StringUtil::Format("CAST(%d AS BIGINT), idt.table_id, idt.schema_version, 0, 0, idt.table_id, "
	                              "idt.schema_version, %s, %s, %s, idt.table_name, %s, %s, %s, %s, %s, "
	                              "%s, %s, %s",
	                              INLINED_TABLE_ROW, n, n, nu, ns, ns, ns, ns, ns, ns, nb, nb),
	           t("ducklake_inlined_data_tables") + " idt"));
	branches.push_back(
	    Branch(StringUtil::Format("CAST(%d AS BIGINT), vw.view_id, 0, 0, 0, vw.view_id, vw.schema_id, %s, %s, "
	                              "vw.view_uuid, vw.view_name, vw.dialect, vw.[sql], vw.column_aliases, "
	                              "%s, %s, %s, %s, %s",
	                              VIEW_ROW, n, n, ns, ns, ns, nb, nb),
	           t("ducklake_view") + " vw WHERE " + Visible("vw")));
	if (v1_1) {
		branches.push_back(
		    Branch(StringUtil::Format("CAST(%d AS BIGINT), vct.view_id, 0, 0, 0, vct.view_id, %s, %s, %s, %s, "
		                              "vct.column_name, vct.[key], vct.[value], %s, %s, %s, %s, %s, %s",
		                              VIEW_COLUMN_TAG_ROW, n, n, n, nu, ns, ns, ns, ns, nb, nb),
		           t("ducklake_view_column_tag") + " vct WHERE " + Visible("vct")));
	}
	branches.push_back(
	    Branch(StringUtil::Format("CAST(%d AS BIGINT), mac.macro_id, 0, 0, 0, mac.schema_id, mac.macro_id, %s, %s, "
	                              "%s, mac.macro_name, %s, %s, %s, %s, %s, %s, %s, %s",
	                              MACRO_ROW, n, n, nu, ns, ns, ns, ns, ns, ns, nb, nb),
	           t("ducklake_macro") + " mac WHERE " + Visible("mac")));
	// the implementations and parameters of the visible macros
	auto visible_macro = [&](const string &alias) {
		return StringUtil::Format("EXISTS (SELECT 1 FROM %s mac WHERE mac.macro_id = %s.macro_id AND %s)",
		                          t("ducklake_macro"), alias, Visible("mac"));
	};
	branches.push_back(
	    Branch(StringUtil::Format("CAST(%d AS BIGINT), imp.macro_id, imp.impl_id, 0, 0, imp.macro_id, imp.impl_id, "
	                              "%s, %s, %s, imp.dialect, imp.[sql], imp.[type], %s, %s, %s, %s, %s, %s",
	                              MACRO_IMPL_ROW, n, n, nu, ns, ns, ns, ns, nb, nb),
	           t("ducklake_macro_impl") + " imp WHERE " + visible_macro("imp")));
	branches.push_back(
	    Branch(StringUtil::Format("CAST(%d AS BIGINT), par.macro_id, par.impl_id, par.column_id, 0, par.macro_id, "
	                              "par.impl_id, par.column_id, %s, %s, par.parameter_name, "
	                              "par.parameter_type, par.default_value, par.default_value_type, %s, %s, "
	                              "%s, %s, %s",
	                              MACRO_PARAMETER_ROW, n, nu, ns, ns, ns, nb, nb),
	           t("ducklake_macro_parameters") + " par WHERE " + visible_macro("par")));
	// partitions and sorts: table_id, then the spec's id, then the key - the base's ORDER BY
	branches.push_back(
	    Branch(StringUtil::Format("CAST(%d AS BIGINT), part.table_id, part.partition_id, pc.partition_key_index, 0, "
	                              "part.partition_id, part.table_id, pc.partition_key_index, "
	                              "pc.column_id, %s, pc.transform, %s, %s, %s, %s, %s, %s, %s, %s",
	                              PARTITION_ROW, nu, ns, ns, ns, ns, ns, ns, nb, nb),
	           t("ducklake_partition_info") + " part JOIN " + t("ducklake_partition_column") +
	               " pc ON pc.partition_id = part.partition_id WHERE " + Visible("part")));
	branches.push_back(
	    Branch(StringUtil::Format("CAST(%d AS BIGINT), srt.table_id, srt.sort_id, se.sort_key_index, 0, srt.sort_id, "
	                              "srt.table_id, se.sort_key_index, %s, %s, se.expression, se.dialect, "
	                              "se.sort_direction, se.null_order, %s, %s, %s, %s, %s",
	                              SORT_ROW, n, nu, ns, ns, ns, nb, nb),
	           t("ducklake_sort_info") + " srt JOIN " + t("ducklake_sort_expression") +
	               " se ON se.sort_id = srt.sort_id WHERE " + Visible("srt")));
	// No ORDER BY here: sorting every row, wide strings and all, was most of the statement's cost on the
	// server (14 ms of CPU a load for 300 tables, against 9 without). DuckDB orders them on arrival.
	return StringUtil::Join(branches, "\nUNION ALL\n");
}

bool IsNull(DataChunk &chunk, idx_t column, idx_t row) {
	return FlatVector::IsNull(chunk.data[column], row);
}

uint64_t Id(DataChunk &chunk, idx_t column, idx_t row) {
	return static_cast<uint64_t>(FlatVector::GetData<int64_t>(chunk.data[column])[row]);
}

string Str(DataChunk &chunk, idx_t column, idx_t row) {
	return FlatVector::GetData<string_t>(chunk.data[column])[row].GetString();
}

bool Flag(DataChunk &chunk, idx_t column, idx_t row) {
	return FlatVector::GetData<bool>(chunk.data[column])[row];
}

//! A string column as the base reads it into a Value: NULL stays NULL.
Value StrValue(DataChunk &chunk, idx_t column, idx_t row) {
	return IsNull(chunk, column, row) ? Value() : Value(Str(chunk, column, row));
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

} // namespace

DuckLakeCatalogInfo MSSQLMetadataManager::GetCatalogForSnapshot(DuckLakeSnapshot snapshot) {
	if (CatalogStatementDisabled()) {
		return DuckLakeMetadataManager::GetCatalogForSnapshot(snapshot);
	}
	auto catalog = LoadCatalogInOneStatement(snapshot);
	if (VerifyCatalogEnabled()) {
		auto base = DuckLakeMetadataManager::GetCatalogForSnapshot(snapshot);
		auto difference = CatalogDifference(base, catalog);
		if (!difference.empty()) {
			throw InvalidInputException(
			    "mssql_ducklake: the one-statement catalog load differs from DuckLake's at snapshot %llu: %s",
			    snapshot.snapshot_id, difference);
		}
	}
	return catalog;
}

DuckLakeCatalogInfo MSSQLMetadataManager::LoadCatalogInOneStatement(DuckLakeSnapshot snapshot) {
	auto &ducklake_catalog = transaction.GetCatalog();
	const bool v1_1 = ducklake_catalog.SupportsV1_1Metadata();
	const auto &base_data_path = ducklake_catalog.DataPath();
	const auto &separator = ducklake_catalog.Separator();

	auto query = StringUtil::Format(
	    "SELECT * FROM mssql_scan_params_unsafe({METADATA_CATALOG_NAME_LITERAL}, %s, {'s': {SNAPSHOT_ID}}, "
	    "'@s BIGINT', columns := {%s}) ORDER BY k, o1, o2, o3, o4",
	    SQLString(CatalogStatement(SchemaIdentifier(), v1_1)), CATALOG_COLUMNS);
	auto result = Query(snapshot, query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to load the DuckLake catalog from SQL Server: ");
	}

	DuckLakeCatalogInfo catalog;
	map<SchemaIndex, idx_t> schema_map;
	map<TableIndex, idx_t> table_map;
	map<TableIndex, idx_t> view_map;
	map<MacroIndex, idx_t> macro_map;
	// tags arrive before the views they belong to (the kinds are ordered), so they wait here
	map<idx_t, vector<DuckLakeTag>> object_tags;
	map<pair<idx_t, idx_t>, vector<DuckLakeTag>> column_tags;
	map<idx_t, vector<DuckLakeViewColumnTag>> view_column_tags;
	// a macro's implementations by impl_id, for its parameters to find
	map<pair<idx_t, idx_t>, idx_t> impl_map;

	auto attach_column_tags = [&](DuckLakeTableInfo &table) {
		std::function<void(vector<DuckLakeColumnInfo> &)> walk = [&](vector<DuckLakeColumnInfo> &columns) {
			for (auto &column : columns) {
				auto entry = column_tags.find({table.id.index, column.id.index});
				if (entry != column_tags.end()) {
					column.tags = entry->second;
				}
				walk(column.children);
			}
		};
		walk(table.columns);
	};

	while (true) {
		auto chunk = result->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		chunk->Flatten();
		for (idx_t row = 0; row < chunk->size(); row++) {
			switch (static_cast<CatalogRowKind>(Id(*chunk, K, row))) {
			case SCHEMA_ROW: {
				DuckLakeSchemaInfo schema;
				schema.id = SchemaIndex(Id(*chunk, I1, row));
				schema.uuid = chunk->GetValue(U, row).ToString();
				schema.name = Str(*chunk, S1, row);
				if (IsNull(*chunk, S2, row)) {
					schema.path = base_data_path;
				} else {
					DuckLakePath path {Str(*chunk, S2, row), !IsNull(*chunk, B1, row) && Flag(*chunk, B1, row)};
					schema.path = DuckLakeMetadataManager::FromRelativePath(path, base_data_path, separator);
				}
				if (v1_1 && !IsNull(*chunk, I2, row)) {
					schema.parent_id = SchemaIndex(Id(*chunk, I2, row));
				}
				schema_map[schema.id] = catalog.schemas.size();
				catalog.schemas.push_back(std::move(schema));
				break;
			}
			case TABLE_ROW: {
				auto table_id = TableIndex(Id(*chunk, I1, row));
				auto &tables = catalog.tables;
				if (tables.empty() || tables.back().id != table_id) {
					DuckLakeTableInfo table_info;
					table_info.id = table_id;
					table_info.schema_id = SchemaIndex(Id(*chunk, I2, row));
					table_info.uuid = chunk->GetValue(U, row).ToString();
					table_info.name = Str(*chunk, S1, row);
					auto schema_entry = schema_map.find(table_info.schema_id);
					if (schema_entry == schema_map.end()) {
						throw InvalidInputException(
						    "Failed to load DuckLake - table with id %d references schema id %d that does not exist",
						    table_info.id.index, table_info.schema_id.index);
					}
					auto &schema = catalog.schemas[schema_entry->second];
					if (IsNull(*chunk, S2, row)) {
						table_info.path = schema.path;
					} else {
						DuckLakePath path {Str(*chunk, S2, row), !IsNull(*chunk, B1, row) && Flag(*chunk, B1, row)};
						table_info.path = DuckLakeMetadataManager::FromRelativePath(path, schema.path, separator);
					}
					table_map[table_id] = tables.size();
					tables.push_back(std::move(table_info));
				}
				auto &table_entry = tables.back();
				if (IsNull(*chunk, I3, row)) {
					throw InvalidInputException(
					    "Failed to load DuckLake - Table entry \"%s\" does not have any columns", table_entry.name);
				}
				DuckLakeColumnInfo column_info;
				column_info.id = FieldIndex(Id(*chunk, I3, row));
				column_info.name = Str(*chunk, S3, row);
				column_info.type = Str(*chunk, S4, row);
				if (!IsNull(*chunk, S5, row)) {
					column_info.initial_default = Value(Str(*chunk, S5, row));
				}
				if (!IsNull(*chunk, S6, row)) {
					auto value = Str(*chunk, S6, row);
					column_info.default_value = value == "NULL" ? Value() : Value(value);
				}
				if (!IsNull(*chunk, S7, row)) {
					column_info.default_value_type = Str(*chunk, S7, row);
				}
				column_info.nulls_allowed = Flag(*chunk, B2, row);
				if (IsNull(*chunk, I4, row)) {
					table_entry.columns.push_back(std::move(column_info));
				} else {
					auto parent_id = FieldIndex(Id(*chunk, I4, row));
					if (!AddChildColumn(table_entry.columns, parent_id, column_info)) {
						throw InvalidInputException(
						    "Failed to load DuckLake - Could not find parent column for column %s", column_info.name);
					}
				}
				break;
			}
			case TAG_ROW:
				// the base's LoadTags: a tag without a value is skipped
				if (!IsNull(*chunk, S2, row)) {
					object_tags[Id(*chunk, I1, row)].push_back({Str(*chunk, S1, row), Str(*chunk, S2, row)});
				}
				break;
			case COLUMN_TAG_ROW:
				if (!IsNull(*chunk, S2, row)) {
					column_tags[{Id(*chunk, I1, row), Id(*chunk, I2, row)}].push_back(
					    {Str(*chunk, S1, row), Str(*chunk, S2, row)});
				}
				break;
			case INLINED_TABLE_ROW: {
				auto entry = table_map.find(TableIndex(Id(*chunk, I1, row)));
				if (entry != table_map.end()) {
					DuckLakeInlinedTableInfo inlined;
					inlined.table_name = Str(*chunk, S1, row);
					inlined.schema_version = Id(*chunk, I2, row);
					catalog.tables[entry->second].inlined_data_tables.push_back(std::move(inlined));
				}
				break;
			}
			case VIEW_ROW: {
				DuckLakeViewInfo view_info;
				view_info.id = TableIndex(Id(*chunk, I1, row));
				view_info.uuid = chunk->GetValue(U, row).ToString();
				view_info.schema_id = SchemaIndex(Id(*chunk, I2, row));
				view_info.name = Str(*chunk, S1, row);
				view_info.dialect = Str(*chunk, S2, row);
				view_info.sql = Str(*chunk, S3, row);
				if (!IsNull(*chunk, S4, row)) {
					view_info.column_aliases = DuckLakeUtil::ParseQuotedList(Str(*chunk, S4, row));
				}
				view_map[view_info.id] = catalog.views.size();
				catalog.views.push_back(std::move(view_info));
				break;
			}
			case VIEW_COLUMN_TAG_ROW: {
				DuckLakeViewColumnTag tag;
				tag.column_name = Str(*chunk, S1, row);
				tag.key = Str(*chunk, S2, row);
				tag.value = StrValue(*chunk, S3, row);
				view_column_tags[Id(*chunk, I1, row)].push_back(std::move(tag));
				break;
			}
			case MACRO_ROW: {
				DuckLakeMacroInfo macro_info;
				macro_info.schema_id = SchemaIndex(Id(*chunk, I1, row));
				macro_info.macro_id = MacroIndex(Id(*chunk, I2, row));
				macro_info.macro_name = Str(*chunk, S1, row);
				macro_map[macro_info.macro_id] = catalog.macros.size();
				catalog.macros.push_back(std::move(macro_info));
				break;
			}
			case MACRO_IMPL_ROW: {
				auto entry = macro_map.find(MacroIndex(Id(*chunk, I1, row)));
				if (entry == macro_map.end()) {
					break;
				}
				auto &implementations = catalog.macros[entry->second].implementations;
				DuckLakeMacroImplementation impl;
				impl.dialect = Str(*chunk, S1, row);
				impl.sql = Str(*chunk, S2, row);
				impl.type = Str(*chunk, S3, row);
				impl_map[{Id(*chunk, I1, row), Id(*chunk, I2, row)}] = implementations.size();
				implementations.push_back(std::move(impl));
				break;
			}
			case MACRO_PARAMETER_ROW: {
				auto macro = macro_map.find(MacroIndex(Id(*chunk, I1, row)));
				auto impl = impl_map.find({Id(*chunk, I1, row), Id(*chunk, I2, row)});
				if (macro == macro_map.end() || impl == impl_map.end()) {
					break;
				}
				DuckLakeMacroParameters parameter;
				parameter.parameter_name = Str(*chunk, S1, row);
				parameter.parameter_type = Str(*chunk, S2, row);
				parameter.default_value = StrValue(*chunk, S3, row);
				parameter.default_value_type = IsNull(*chunk, S4, row) ? string() : Str(*chunk, S4, row);
				catalog.macros[macro->second].implementations[impl->second].parameters.push_back(std::move(parameter));
				break;
			}
			case PARTITION_ROW: {
				auto table_id = TableIndex(Id(*chunk, I2, row));
				auto &partitions = catalog.partitions;
				if (partitions.empty() || partitions.back().table_id != table_id) {
					DuckLakePartitionInfo partition_info;
					partition_info.id = Id(*chunk, I1, row);
					partition_info.table_id = table_id;
					partitions.push_back(std::move(partition_info));
				}
				DuckLakePartitionFieldInfo field;
				field.partition_key_index = Id(*chunk, I3, row);
				field.field_id = FieldIndex(Id(*chunk, I4, row));
				field.transform = Str(*chunk, S1, row);
				partitions.back().fields.push_back(std::move(field));
				break;
			}
			case SORT_ROW: {
				auto table_id = TableIndex(Id(*chunk, I2, row));
				auto &sorts = catalog.sorts;
				if (sorts.empty() || sorts.back().table_id != table_id) {
					DuckLakeSortInfo sort_info;
					sort_info.id = Id(*chunk, I1, row);
					sort_info.table_id = table_id;
					sorts.push_back(std::move(sort_info));
				}
				DuckLakeSortFieldInfo field;
				field.sort_key_index = Id(*chunk, I3, row);
				field.expression = Str(*chunk, S1, row);
				field.dialect = Str(*chunk, S2, row);
				field.sort_direction =
				    StringUtil::CIEquals(Str(*chunk, S3, row), "DESC") ? OrderType::DESCENDING : OrderType::ASCENDING;
				field.null_order = StringUtil::CIEquals(Str(*chunk, S4, row), "NULLS_FIRST")
				                       ? OrderByNullType::NULLS_FIRST
				                       : OrderByNullType::NULLS_LAST;
				sorts.back().fields.push_back(std::move(field));
				break;
			}
			default:
				throw InternalException("mssql_ducklake: unknown row kind in the catalog load");
			}
		}
	}

	for (auto &table : catalog.tables) {
		auto tags = object_tags.find(table.id.index);
		if (tags != object_tags.end()) {
			table.tags = tags->second;
		}
		attach_column_tags(table);
	}
	for (auto &view : catalog.views) {
		auto tags = object_tags.find(view.id.index);
		if (tags != object_tags.end()) {
			view.tags = tags->second;
		}
		auto vct = view_column_tags.find(view.id.index);
		if (vct != view_column_tags.end()) {
			view.column_tags = vct->second;
		}
	}
	return catalog;
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
