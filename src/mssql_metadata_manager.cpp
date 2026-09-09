#include "mssql_metadata_manager.hpp"

#include "common/ducklake_types.hpp"
#include "common/ducklake_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/main/database.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_metadata_info.hpp"
#include "storage/ducklake_staged_commit.hpp"
#include "storage/ducklake_transaction_changes.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

// the out-of-line definition the pre-C++17 build needs, since the constant is passed by reference
constexpr const char *MSSQLMetadataManager::VARCHAR_COLLATION;
constexpr const char *MSSQLMetadataManager::SHAPE_VERSION_PROPERTY;

MSSQLMetadataManager::MSSQLMetadataManager(DuckLakeTransaction &transaction) : DuckLakeMetadataManager(transaction) {
}

namespace {

//! DuckLake's commit loop, on a retry, asks one question that reads ducklake_snapshot TWICE:
//!
//!     FROM ducklake_snapshot WHERE snapshot_id = (SELECT MAX(snapshot_id) FROM ducklake_snapshot)
//!
//! Through an attached catalog those are two separate SELECTs to the server, materialised
//! independently on the pinned connection, with no consistent read between them. A commit landing in
//! the gap makes them disagree - measured with MSSQL_DEBUG=2, the subquery came back with 13
//! snapshots while the outer scan had seen 12 - so the predicate matches nothing, the snapshot
//! branch of the UNION ALL is empty, the first row of the result is a statistics row, and DuckLake's
//! parser reads its NULL snapshot_id as an idx_t: "Calling GetValueInternal on a value that is
//! NULL". Measured over 14 alternating rounds of four concurrent writers, this lost a writer in 6 of
//! them; the postgres backend, whose scans share one transaction snapshot, lost none (specs/007 D1).
//!
//! Kept verbatim so that a ducklake bump editing this query is caught at attach - see
//! ProbeServerCapabilities - rather than silently disabling the rewrite below.
constexpr const char *DUCKLAKE_CONFLICT_CHECK_QUERY = R"(
SELECT
    snapshot_id,
    schema_version,
    next_catalog_id,
    next_file_id,
    COALESCE((
            SELECT STRING_AGG(changes_made, ',')
            FROM {METADATA_CATALOG}.ducklake_snapshot_changes c
            WHERE c.snapshot_id > {SNAPSHOT_ID}
            ),'') AS changes,
    NULL AS table_id,
    NULL AS column_id,
    NULL AS record_count,
    NULL AS next_row_id,
    NULL AS file_size_bytes,
    NULL AS contains_null,
    NULL AS contains_nan,
    NULL AS min_value,
    NULL AS max_value,
    NULL AS extra_stats
    FROM {METADATA_CATALOG}.ducklake_snapshot
    WHERE snapshot_id = (
        SELECT MAX(snapshot_id)
        FROM {METADATA_CATALOG}.ducklake_snapshot)
UNION ALL
SELECT
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    table_id,
    column_id,
    record_count,
    next_row_id,
    file_size_bytes,
    contains_null,
    contains_nan,
    min_value,
    max_value,
    extra_stats
FROM {METADATA_CATALOG}.ducklake_table_stats
LEFT JOIN {METADATA_CATALOG}.ducklake_table_column_stats
    USING (table_id)
WHERE record_count IS NOT NULL
    AND file_size_bytes IS NOT NULL
ORDER BY table_id NULLS FIRST;
	)";

//! The replacement. Reading the table once removes the disagreement rather than papering over it:
//! one scan cannot contradict itself, whatever commits in parallel. Everything else is untouched -
//! same columns in the same order, same placeholders - because DuckLake's parser depends on all of
//! it, and the base substitutes {METADATA_CATALOG} and {SNAPSHOT_ID} here exactly as it would there.
constexpr const char *MSSQL_CONFLICT_CHECK_QUERY = R"(
SELECT
    snapshot_id,
    schema_version,
    next_catalog_id,
    next_file_id,
    COALESCE((
            SELECT STRING_AGG(changes_made, ',')
            FROM {METADATA_CATALOG}.ducklake_snapshot_changes c
            WHERE c.snapshot_id > {SNAPSHOT_ID}
            ),'') AS changes,
    NULL AS table_id,
    NULL AS column_id,
    NULL AS record_count,
    NULL AS next_row_id,
    NULL AS file_size_bytes,
    NULL AS contains_null,
    NULL AS contains_nan,
    NULL AS min_value,
    NULL AS max_value,
    NULL AS extra_stats
    FROM (
        SELECT * FROM {METADATA_CATALOG}.ducklake_snapshot ORDER BY snapshot_id DESC LIMIT 1
    ) latest_snapshot
UNION ALL
SELECT
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    table_id,
    column_id,
    record_count,
    next_row_id,
    file_size_bytes,
    contains_null,
    contains_nan,
    min_value,
    max_value,
    extra_stats
FROM {METADATA_CATALOG}.ducklake_table_stats
LEFT JOIN {METADATA_CATALOG}.ducklake_table_column_stats
    USING (table_id)
WHERE record_count IS NOT NULL
    AND file_size_bytes IS NOT NULL
ORDER BY table_id NULLS FIRST;
	)";

} // namespace

//===--------------------------------------------------------------------===//
// The inlining type matrix (specs/004 D4)
//===--------------------------------------------------------------------===//

bool MSSQLMetadataManager::TypeIsNativelySupported(const LogicalType &type) {
	switch (type.id()) {
	// SQL Server has no NaN and no infinities, so a float column cannot round trip
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	// DATETIME2/TIME(7) resolve to 100ns, so nanoseconds are lossy
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIME_NS:
	// wider than DECIMAL(38, 0), the widest exact numeric SQL Server has
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	// no server-side equivalent; DuckLake's canonical text is the storage
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::BIT:
	case LogicalTypeId::ENUM:
	case LogicalTypeId::VARIANT:
	case LogicalTypeId::GEOMETRY:
	// nested types are stored as text by DuckLake itself
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::MAP:
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY:
	case LogicalTypeId::UNION:
		return false;
	default:
		return true;
	}
}

bool MSSQLMetadataManager::SupportsInlining(const LogicalType &type) {
	if (type.id() == LogicalTypeId::VARIANT) {
		// DuckLake throws mid-commit for a VARIANT it cannot store natively rather than falling back
		// to a data file, so the column has to be refused before the write starts
		return false;
	}
	return DuckLakeMetadataManager::SupportsInlining(type);
}

string MSSQLMetadataManager::GetColumnTypeInternal(const LogicalType &column_type) {
	// DuckDB names, not T-SQL ones. DuckLake writes this string into the `CAST(<value> AS <type>)`
	// it puts in the commit batch, and that batch is executed by duckdb against the attached
	// catalog - a T-SQL name there fails to parse ("Type with name DATETIME2 does not exist").
	// The server-side spelling belongs to TSQLColumnType, which only our own DDL uses.
	if (!TypeIsNativelySupported(column_type)) {
		return "VARCHAR";
	}
	return DuckLakeMetadataManager::GetColumnTypeInternal(column_type);
}

string MSSQLMetadataManager::TSQLColumnType(const LogicalType &column_type) const {
	switch (column_type.id()) {
	case LogicalTypeId::BOOLEAN:
		return "BIT";
	// T-SQL's TINYINT is unsigned, so the signed one needs a wider column and ours fits it exactly
	case LogicalTypeId::TINYINT:
		return "SMALLINT";
	case LogicalTypeId::UTINYINT:
		return "TINYINT";
	case LogicalTypeId::SMALLINT:
		return "SMALLINT";
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::INTEGER:
		return "INT";
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::BIGINT:
		return "BIGINT";
	case LogicalTypeId::DECIMAL:
		return StringUtil::Format("DECIMAL(%d, %d)", DecimalType::GetWidth(column_type),
		                          DecimalType::GetScale(column_type));
	case LogicalTypeId::BLOB:
		return "VARBINARY(MAX)";
	case LogicalTypeId::DATE:
		return "DATE";
	case LogicalTypeId::TIME:
		return "TIME(6)";
	case LogicalTypeId::TIMESTAMP:
		return "DATETIME2(6)";
	case LogicalTypeId::TIMESTAMP_MS:
		return "DATETIME2(3)";
	case LogicalTypeId::TIMESTAMP_SEC:
		return "DATETIME2(0)";
	case LogicalTypeId::TIMESTAMP_TZ:
		return "DATETIMEOFFSET(6)";
	case LogicalTypeId::UUID:
		return "UNIQUEIDENTIFIER";
	default:
		// The types above that SQL Server cannot hold exactly, and the nested ones DuckLake stores
		// as text anyway. MAX rather than a bound, because DuckLake states no length and a bare
		// VARCHAR is VARCHAR(1) in T-SQL; the collation is explicit rather than inherited, because a
		// database's own is often a legacy CI_AS one.
		return StringUtil::Format("VARCHAR(MAX) COLLATE %s", VARCHAR_COLLATION);
	}
}

//===--------------------------------------------------------------------===//
// Talking to the server
//===--------------------------------------------------------------------===//

string MSSQLMetadataManager::SchemaIdentifier() const {
	return DuckLakeUtil::SQLIdentifierToString(transaction.GetCatalog().MetadataSchemaName());
}

string MSSQLMetadataManager::CatalogLiteral() const {
	return DuckLakeUtil::SQLLiteralToString(transaction.GetCatalog().MetadataDatabaseName());
}

void MSSQLMetadataManager::RunOn(Connection &connection, const string &tsql, const string &context) {
	auto result = connection.Query(StringUtil::Format("SELECT mssql_exec(%s, %s)", CatalogLiteral(), SQLString(tsql)));
	if (result->HasError()) {
		result->GetErrorObject().Throw(context);
	}
}

void MSSQLMetadataManager::RunServerSide(const string &tsql, const string &context) {
	RunOn(transaction.GetConnection(), tsql, context);
}

void MSSQLMetadataManager::RunServerSideOutsideTransaction(const string &tsql, const string &context) {
	// A table this transaction creates but has not committed is locked against the metadata query
	// the mssql extension runs to discover it - and that query takes its own connection, so it waits
	// on us until it times out. Created in autocommit the table is visible at once and holds no lock.
	auto client_context = transaction.context.lock();
	if (!client_context) {
		throw InternalException("MSSQLMetadataManager: the client context is gone");
	}
	Connection connection(*client_context->db);
	RunOn(connection, tsql, context);
}

void MSSQLMetadataManager::ClearCache() {
	// The mssql extension caches catalog metadata, and a table created behind its back through
	// mssql_exec is invisible to the duckdb reads that follow - they miss it silently rather than
	// failing. DuckLake tracks when that has happened and calls this at the end of a commit; issuing
	// it earlier deadlocks, because the refresh queries the catalog on a second connection and waits
	// on the schema locks this transaction holds.
	//
	// Named down to the table wherever we know it, because a schema-wide clear is not cheap: the
	// catalog holds 23 tables plus an inlined table per lake table, and re-reading their columns and
	// keys measured 39 round trips - more than the commit that triggered it (specs/005 D7). There are
	// exactly two places a table appears behind the extension's back, and both record the name here.
	// Nothing recorded means we do not know what changed - the attach-time clear - and the schema is
	// the honest answer then.
	auto &connection = transaction.GetConnection();
	auto schema = DuckLakeUtil::SQLLiteralToString(transaction.GetCatalog().MetadataSchemaName());
	vector<string> calls;
	if (tables_pending_cache_refresh.empty()) {
		calls.push_back(StringUtil::Format("SELECT mssql_invalidate_cache(%s, %s)", CatalogLiteral(), schema));
	} else {
		for (auto &table_name : tables_pending_cache_refresh) {
			calls.push_back(StringUtil::Format("SELECT mssql_invalidate_cache(%s, %s, %s)", CatalogLiteral(), schema,
			                                   DuckLakeUtil::SQLLiteralToString(table_name)));
		}
	}
	tables_pending_cache_refresh.clear();
	for (auto &call : calls) {
		auto result = connection.Query(call);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to refresh the SQL Server catalog cache: ");
		}
	}
}

void MSSQLMetadataManager::InvalidateTableCache(const string &table_name) {
	auto &connection = transaction.GetConnection();
	auto result = connection.Query(
	    StringUtil::Format("SELECT mssql_invalidate_cache(%s, %s, %s)", CatalogLiteral(),
	                       DuckLakeUtil::SQLLiteralToString(transaction.GetCatalog().MetadataSchemaName()),
	                       DuckLakeUtil::SQLLiteralToString(table_name)));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to refresh the SQL Server catalog cache: ");
	}
}

string MSSQLMetadataManager::GetInlinedDeletionTableName(TableIndex table_id, DuckLakeSnapshot snapshot,
                                                         bool create_if_not_exists) {
	auto table_name = DuckLakeMetadataManager::GetInlinedDeletionTableName(table_id, snapshot, create_if_not_exists);
	if (create_if_not_exists && !table_name.empty()) {
		// The base may have created it or found it cached; we cannot tell, and recording a name that
		// did not need refreshing costs one precise invalidation, while missing one costs correctness.
		tables_pending_cache_refresh.push_back(table_name);
	}
	return table_name;
}

//===--------------------------------------------------------------------===//
// Initialization (specs/004 D3)
//===--------------------------------------------------------------------===//

bool MSSQLMetadataManager::CatalogShapeIsCurrent() {
	// One cheap question instead of two batches of DDL. Attaching an existing catalog is on the hot
	// path - every transaction pays for whatever happens here - and the shaping is only ever needed
	// once per catalog per version of this extension.
	//
	// A version rather than the presence of one constraint. The marker used to be the last key the
	// shaping adds, which answered "some build of this extension shaped this catalog" - so a catalog
	// shaped by an older one, whose column types and indexes are not what the current build wants,
	// read as current and was left alone. The stamp is written last, after the DDL that earns it.
	auto &connection = transaction.GetConnection();
	// the inner statement travels inside a duckdb string literal, so each of its own quotes is
	// doubled once - the same shape the collation probe above uses
	auto schema_name = StringUtil::Replace(transaction.GetCatalog().MetadataSchemaName(), "'", "''''");
	auto result = connection.Query(StringUtil::Format(
	    "SELECT shape FROM mssql_scan(%s, 'SELECT TRY_CAST(CAST(value AS VARCHAR(32)) AS BIGINT) AS shape "
	    "FROM sys.extended_properties WHERE class = 3 AND major_id = SCHEMA_ID(''%s'') AND name = ''%s''')",
	    CatalogLiteral(), schema_name, SHAPE_VERSION_PROPERTY));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to inspect the DuckLake catalog on SQL Server: ");
	}
	auto row = result->Fetch();
	if (!row || row->size() == 0 || row->GetValue(0, 0).IsNull()) {
		return false;
	}
	return row->GetValue(0, 0).GetValue<int64_t>() >= SHAPE_VERSION;
}

//! A T-SQL string literal, or NULL - the value travels inside the batch mssql_exec runs.
static string TSQLLiteral(const string &value) {
	return "N'" + StringUtil::Replace(value, "'", "''") + "'";
}

void MSSQLMetadataManager::EnsureCatalogShape() {
	const string schema = SchemaIdentifier();
	const string schema_literal = DuckLakeUtil::SQLLiteralToString(transaction.GetCatalog().MetadataSchemaName());

	// Primary keys. DuckLake declares a few itself; the rest are ours, and they are what make the
	// catalog writable at all: the mssql extension builds a row identity out of the primary key, and
	// without one it refuses every UPDATE and DELETE - which commits, expiry, cleanup and compaction
	// are full of. The list is every table DuckLake updates or deletes from.
	const vector<pair<string, string>> keys = {
	    {"ducklake_metadata", "[key]"},
	    {"ducklake_table_stats", "table_id"},
	    {"ducklake_table_column_stats", "table_id, column_id"},
	    {"ducklake_table", "table_id, begin_snapshot"},
	    {"ducklake_view", "view_id, begin_snapshot"},
	    {"ducklake_column", "table_id, column_id, begin_snapshot"},
	    {"ducklake_tag", "object_id, begin_snapshot, [key]"},
	    {"ducklake_column_tag", "table_id, column_id, begin_snapshot, [key]"},
	    {"ducklake_partition_info", "partition_id"},
	    {"ducklake_partition_column", "partition_id, partition_key_index"},
	    {"ducklake_sort_info", "sort_id"},
	    {"ducklake_sort_expression", "sort_id, sort_key_index"},
	    {"ducklake_macro", "macro_id, begin_snapshot"},
	    {"ducklake_macro_impl", "macro_id, impl_id"},
	    {"ducklake_macro_parameters", "macro_id, impl_id, column_id"},
	    {"ducklake_inlined_data_tables", "table_id, schema_version"},
	    {"ducklake_files_scheduled_for_deletion", "data_file_id"},
	    {"ducklake_file_column_stats", "data_file_id, column_id"},
	    {"ducklake_file_variant_stats", "data_file_id, column_id, variant_path"},
	    {"ducklake_file_partition_value", "data_file_id, partition_key_index"},
	    {"ducklake_column_mapping", "mapping_id"},
	    {"ducklake_name_mapping", "mapping_id, column_id"},
	    {"ducklake_schema_versions", "begin_snapshot, schema_version"},
	};
	// The tag tables and the metadata table key on a name rather than an id. A primary key cannot be
	// over MAX, so those are capped - 200 bytes of UTF-8 is a long name and well inside the
	// 900-byte index limit.
	auto key_column_type = [&](const string &name) {
		if (name == "[key]" || name == "variant_path") {
			return StringUtil::Format("VARCHAR(200) COLLATE %s", VARCHAR_COLLATION);
		}
		return string("BIGINT");
	};

	string columns_ddl;
	string constraints_ddl;
	for (auto &entry : keys) {
		for (auto &column : StringUtil::Split(entry.second, ',')) {
			auto name = column;
			StringUtil::Trim(name);
			// a key column has to be NOT NULL, and DuckLake declares none of them so
			columns_ddl += StringUtil::Format("ALTER TABLE %s.%s ALTER COLUMN %s %s NOT NULL;\n", schema, entry.first,
			                                  name, key_column_type(name));
		}
		// idempotent, so that an existing catalog can be brought up to shape on attach and a run
		// that failed partway can be resumed
		constraints_ddl += StringUtil::Format(
		    "IF NOT EXISTS (SELECT 1 FROM sys.key_constraints WHERE name = 'pk_%s' AND schema_id = SCHEMA_ID(%s)) "
		    "ALTER TABLE %s.%s ADD CONSTRAINT pk_%s PRIMARY KEY (%s);\n",
		    entry.first, schema_literal, schema, entry.first, entry.first, entry.second);
	}

	// The statistics DuckLake compares server-side. Its min/max values are the bytes DuckDB computed
	// in UTF-8 order, and a filter it pushes down is answered by the column's collation - so a
	// linguistic one prunes wrongly and drops rows from a result. BIN2 over UTF-8 is DuckDB's order.
	const vector<pair<string, string>> stats_columns = {
	    {"ducklake_file_column_stats", "min_value"},  {"ducklake_file_column_stats", "max_value"},
	    {"ducklake_table_column_stats", "min_value"}, {"ducklake_table_column_stats", "max_value"},
	    {"ducklake_file_variant_stats", "min_value"}, {"ducklake_file_variant_stats", "max_value"},
	};
	for (auto &entry : stats_columns) {
		columns_ddl += StringUtil::Format("ALTER TABLE %s.%s ALTER COLUMN %s VARCHAR(MAX) COLLATE %s;\n", schema,
		                                  entry.first, entry.second, VARCHAR_COLLATION);
	}

	// A partition value is bounded so it can be an index key. DuckLake declares it an unbounded
	// VARCHAR, which lands as a LOB, and a LOB can be neither an index key nor an INCLUDE - so the
	// pruning predicate below would have nothing to seek. 200 bytes of UTF-8 is far more than a
	// partition key ever is (a date, a tenant, a bucket) and well inside the 900-byte index limit;
	// a longer one is refused by the server rather than silently truncated.
	columns_ddl += StringUtil::Format("ALTER TABLE %s.ducklake_file_partition_value ALTER COLUMN partition_value "
	                                  "VARCHAR(200) COLLATE %s NULL;\n",
	                                  schema, VARCHAR_COLLATION);

	// Everything else DuckLake declared as a string. mssql maps DuckDB's VARCHAR to NVARCHAR, which
	// is UTF-16: two bytes per character for catalog content that is paths, type names and
	// identifiers, and a linguistic case-insensitive collation over them. Neither is what this
	// catalog wants. VARCHAR under a UTF-8 collation stores the same text in half the space for that
	// content, and BIN2 is DuckDB's own comparison - DuckDB has no case-insensitive string compare,
	// so a server that does is the one behaving differently.
	//
	// Generated from sys.columns rather than listed here: the list is DuckLake's, it moves with every
	// submodule bump, and a column added upstream would otherwise silently keep the wrong type. Once
	// converted the sweep matches nothing, so it costs a single statement on every later attach - but
	// the FIRST attach after an upgrade converts in place, and ALTER COLUMN rewrites the table, so
	// that one attach walks the whole catalog (specs/006 D4 measures it).
	//
	// `ducklake%` in the metadata schema is the extension's namespace, not a guess: DuckLake creates
	// and drops tables under that prefix there by itself (every `ducklake_inlined_data_<t>_<v>`), so
	// a table of someone else's answering to it would already be colliding with DuckLake. The scope
	// is deliberately the same one the integration suite's reset uses.
	//
	// Column-by-column because ALTER COLUMN cannot restate a whole table, and nullability has to be
	// restated or the column silently becomes nullable.
	columns_ddl += StringUtil::Format(R"(
DECLARE @widen NVARCHAR(MAX) = N'';
SELECT @widen += N'ALTER TABLE ' + QUOTENAME(sch.name) + N'.' + QUOTENAME(t.name)
               + N' ALTER COLUMN ' + QUOTENAME(c.name) + N' VARCHAR(MAX) COLLATE %s'
               + CASE WHEN c.is_nullable = 0 THEN N' NOT NULL' ELSE N' NULL' END + N';'
FROM sys.columns c
JOIN sys.tables t ON t.object_id = c.object_id
JOIN sys.schemas sch ON sch.schema_id = t.schema_id
JOIN sys.types ty ON ty.user_type_id = c.user_type_id
WHERE sch.name = %s AND t.name LIKE 'ducklake%%' AND ty.name IN ('nvarchar', 'nchar', 'ntext');
EXEC sp_executesql @widen;
)",
	                                  VARCHAR_COLLATION, schema_literal);

	// Indexes on the condition almost every DuckLake read carries. Neither the postgres nor the
	// sqlite manager has indexes here at all.
	//
	// The two file tables are keyed on the whole visibility condition rather than filtered on part of
	// it. A filtered `WHERE end_snapshot IS NULL` index serves a read of the current state and, by
	// construction, nothing else: a read at an older snapshot wants the rows whose end_snapshot is
	// SET, which the filter excludes, so it falls back to scanning the table - and that scan grows
	// with the whole table rather than with the answer. Measured over 300,000 files across 1000
	// tables, half of them superseded, with the rounds alternated:
	//
	//     filtered only       current 0.001s   at an old snapshot 0.006 - 0.008s
	//     unfiltered only     current 0.001s   at an old snapshot 0.001s
	//     both                current 0.001s   at an old snapshot 0.001s
	//
	// One unfiltered index does what two do, so this replaces rather than adds. Time travel is the
	// obvious beneficiary; so is every maintenance function that walks history.
	const vector<pair<string, string>> visibility_indexes = {
	    {"ducklake_data_file", "table_id, begin_snapshot, end_snapshot"},
	    {"ducklake_delete_file", "table_id, begin_snapshot, end_snapshot"},
	};
	// Every check below is scoped to this catalog's schema. `sys.indexes.name` is unique per table,
	// not per database, so two lakes in two schemas of one database name their indexes identically -
	// and an unscoped existence check would let the second one skip creating indexes it has not got.
	// The primary keys above are scoped for the same reason.
	auto index_exists = [&](const string &index_name) {
		return StringUtil::Format("SELECT 1 FROM sys.indexes i JOIN sys.objects o ON o.object_id = i.object_id "
		                          "WHERE i.name = '%s' AND o.schema_id = SCHEMA_ID(%s)",
		                          index_name, schema_literal);
	};
	for (auto &entry : visibility_indexes) {
		const auto visible = "ix_" + entry.first + "_visible";
		constraints_ddl += StringUtil::Format("IF NOT EXISTS (%s) CREATE INDEX %s ON %s.%s(%s);\n",
		                                      index_exists(visible), visible, schema, entry.first, entry.second);
		// the filtered index this replaces, left behind by an older build of this extension
		const auto live = "ix_" + entry.first + "_live";
		constraints_ddl += StringUtil::Format("IF EXISTS (%s) DROP INDEX %s ON %s.%s;\n", index_exists(live), live,
		                                      schema, entry.first);
	}

	// The rest keep the filtered form: their hot reads are the catalog load, which asks for the
	// current state and nothing else, and they are small enough that the difference was not worth
	// measuring either way.
	const vector<pair<string, string>> live_indexes = {
	    {"ducklake_table", "schema_id"},
	    {"ducklake_column", "table_id"},
	    {"ducklake_view", "schema_id"},
	};
	for (auto &entry : live_indexes) {
		const auto live = "ix_" + entry.first + "_live";
		constraints_ddl +=
		    StringUtil::Format("IF NOT EXISTS (%s) CREATE INDEX %s ON %s.%s(%s) WHERE end_snapshot IS NULL;\n",
		                       index_exists(live), live, schema, entry.first, entry.second);
	}

	// The per-column statistics a filtered read prunes with: the largest table in the catalog, a row
	// per file per column, asked for as `column_id = ? AND table_id = ?`. The primary key is
	// (data_file_id, column_id) and its leading column is not in that predicate, so nothing served
	// it. Measured over 1000 tables holding 1.23M stats rows between them, asking one table for one
	// column: 0.001s against 0.015s, and the server seeks this index rather than scanning the key.
	//
	// The distribution is what makes it matter, and measuring it wrong is easy: with every row under
	// a single table_id the same query matches 30,000 rows, a scan is competitive, and the index
	// measures as worthless. A real catalog spreads its rows over its tables.
	//
	// Keys only, no INCLUDE. min_value and max_value are VARCHAR(MAX) - DuckLake declares them
	// without a length and bounds nothing it writes - and a MAX column is a LOB, so it can be
	// neither an index key nor something worth duplicating into one. No filtered form either: stats
	// have no end_snapshot, they belong to a file and the file is what expires.
	const string stats_lookup = "ix_ducklake_file_column_stats_lookup";
	constraints_ddl += StringUtil::Format(
	    "IF NOT EXISTS (%s) CREATE INDEX %s ON %s.ducklake_file_column_stats(table_id, column_id);\n",
	    index_exists(stats_lookup), stats_lookup, schema);

	// Partition pruning, which is the whole reason to partition: DuckLake turns a filter on a
	// partition key into
	//   SELECT data_file_id FROM ducklake_file_partition_value
	//   WHERE table_id = ? AND partition_key_index = ? AND partition_value IN (...)
	// (ducklake_metadata_manager.cpp). The primary key is (data_file_id, partition_key_index) and its
	// leading column is not in that predicate, so nothing served it - the same shape that made the
	// file-column-stats index worth 15x. All three predicate columns are in the key, so the server
	// seeks and reads nothing it does not return; this is what the VARCHAR(200) above is for.
	const string partition_lookup = "ix_ducklake_file_partition_value_lookup";
	constraints_ddl += StringUtil::Format("IF NOT EXISTS (%s) CREATE INDEX %s ON "
	                                      "%s.ducklake_file_partition_value(table_id, partition_key_index, "
	                                      "partition_value);\n",
	                                      index_exists(partition_lookup), partition_lookup, schema);

	// Last, and only if everything above succeeded: the version stamp CatalogShapeIsCurrent reads.
	// It is what lets a catalog shaped by an older build of this extension be brought up to the
	// current shape - the DDL is all idempotent, so the stamp is the only thing that decides whether
	// it is worth running at all.
	constraints_ddl += StringUtil::Format(R"(
IF EXISTS (SELECT 1 FROM sys.extended_properties WHERE class = 3 AND major_id = SCHEMA_ID(%s) AND name = '%s')
    EXEC sp_updateextendedproperty @name = N'%s', @value = N'%d', @level0type = N'SCHEMA', @level0name = %s;
ELSE
    EXEC sp_addextendedproperty @name = N'%s', @value = N'%d', @level0type = N'SCHEMA', @level0name = %s;
)",
	                                      schema_literal, SHAPE_VERSION_PROPERTY, SHAPE_VERSION_PROPERTY, SHAPE_VERSION,
	                                      schema_literal, SHAPE_VERSION_PROPERTY, SHAPE_VERSION, schema_literal);

	// Two batches: inside one, a column's new NOT NULL is not yet visible to the constraint that
	// needs it, and the server answers "cannot define PRIMARY KEY on a nullable column".
	RunServerSide(columns_ddl, "Failed to prepare the DuckLake catalog columns for SQL Server: ");
	RunServerSide(constraints_ddl, "Failed to key and index the DuckLake catalog for SQL Server: ");
}

void MSSQLMetadataManager::InitializeDuckLake(bool has_explicit_schema, DuckLakeEncryption encryption) {
	auto &connection = transaction.GetConnection();
	// The catalog's string columns are stored with a UTF-8 collation, so the server has to have one.
	// Asking for the collation itself rather than the version: Azure SQL Database reports major
	// version 12 while supporting it, and the version was only ever a proxy for this question.
	auto probe = connection.Query(StringUtil::Format(
	    "SELECT collations FROM mssql_scan(%s, 'SELECT COUNT(*) AS collations FROM sys.fn_helpcollations() "
	    "WHERE name = ''%s''')",
	    CatalogLiteral(), VARCHAR_COLLATION));
	if (probe->HasError()) {
		probe->GetErrorObject().Throw("Failed to ask SQL Server for its collations: ");
	}
	auto row = probe->Fetch();
	if (!row || row->size() == 0 || row->GetValue(0, 0).IsNull() || row->GetValue(0, 0).GetValue<int64_t>() == 0) {
		throw NotImplementedException(
		    "This SQL Server has no %s collation, which a DuckLake catalog needs to store its strings without loss. "
		    "UTF-8 collations arrived in SQL Server 2019.",
		    VARCHAR_COLLATION);
	}

	// DuckLake's own DDL first, through duckdb - it owns the shape of its catalog, and reproducing
	// it here would be a copy to re-audit on every submodule bump.
	DuckLakeMetadataManager::InitializeDuckLake(has_explicit_schema, encryption);
	EnsureCatalogShape();
}

//===--------------------------------------------------------------------===//
// Phase 2: staging a commit on the server (specs/005)
//===--------------------------------------------------------------------===//

//! The server-side apply. It covers a commit that adds data files and nothing else - every other
//! staged shape (delete files, inlined data, compactions, name maps) still goes to the client loop,
//! and the caller checks that before using this (specs/005 D4).
//!
//! It is a batch rather than a stored procedure. A procedure would be the
//! natural home for it - versioned, compiled once - but calling one desynchronizes the mssql
//! extension's TDS parser (hugr-lab/mssql-extension#323: RETURNSTATUS is read as if it carried a
//! length), and a desynchronized connection cannot be recovered mid-commit. A batch produces no
//! RETURNSTATUS. Revisit when that lands.
static string CommitBatchSql(const string &schema, const string &collation, int64_t schema_version,
                             const string &author, const string &commit_message, const string &commit_extra_info) {
	// Named substitution, not positional formatting: this statement names the schema a dozen times,
	// and a miscounted argument list is a runtime exception whose message is the SQL itself.
	string sql = R"(
    SET NOCOUNT ON;
    SET XACT_ABORT ON;
    DECLARE @schema_version BIGINT = {SCHEMA_VERSION};
    DECLARE @author NVARCHAR(MAX) = {AUTHOR};
    DECLARE @commit_message NVARCHAR(MAX) = {MESSAGE};
    DECLARE @commit_extra_info NVARCHAR(MAX) = {EXTRA};
    DROP TABLE IF EXISTS #ducklake_commit_result;
    CREATE TABLE #ducklake_commit_result(snapshot_id BIGINT, schema_version BIGINT, had_flushes BIT);

    -- Allocate the snapshot. UPDLOCK/HOLDLOCK is what serializes two commits racing for the same
    -- id: the second waits here rather than failing later on the primary key, which is the whole
    -- point of doing this on the server.
    DECLARE @snapshot_id BIGINT, @schema_ver BIGINT, @next_catalog_id BIGINT, @next_file_id BIGINT;
    SELECT TOP 1
        @snapshot_id = snapshot_id + 1,
        @schema_ver = schema_version,
        @next_catalog_id = next_catalog_id,
        @next_file_id = next_file_id
    FROM {SCHEMA}.ducklake_snapshot WITH (UPDLOCK, HOLDLOCK)
    ORDER BY snapshot_id DESC;

    IF @schema_version >= 0 AND @schema_version <> @schema_ver
    BEGIN
        -- the catalog moved under this transaction; the client has to rebuild and retry
        THROW 51000, 'ducklake_commit: schema version conflict', 1;
    END

    -- Hand the staged files their catalog ids, in a stable order.
    DROP TABLE IF EXISTS #ducklake_commit_files;
    CREATE TABLE #ducklake_commit_files (local_id BIGINT PRIMARY KEY, data_file_id BIGINT, table_id BIGINT);
    INSERT INTO #ducklake_commit_files (local_id, data_file_id, table_id)
    SELECT data_file_id,
           @next_file_id + ROW_NUMBER() OVER (ORDER BY data_file_id) - 1,
           table_id
    FROM #ducklake_staged_data_file;

    -- Row ids are assigned HERE, not by the client. A staged file carries a row_id_start only when
    -- it is a flush of inlined data, which keeps the ids those rows already had; every other file
    -- is a fresh insert and takes the table's current next_row_id, files in staging order (the
    -- staged id increases with file_order within a table, so it reproduces the client's order).
    -- Left NULL - as the first cut left it - the catalog reads back a NULL next_row_id and every
    -- later scan of the table dies estimating cardinality.
    DROP TABLE IF EXISTS #ducklake_assigned_row_id;
    SELECT s.data_file_id AS local_id,
           COALESCE(s.row_id_start,
                    COALESCE(ts.next_row_id, 0)
                    + SUM(CASE WHEN s.partial_max IS NULL THEN s.record_count ELSE 0 END)
                          OVER (PARTITION BY s.table_id ORDER BY s.data_file_id ROWS UNBOUNDED PRECEDING)
                    - CASE WHEN s.partial_max IS NULL THEN s.record_count ELSE 0 END) AS row_id_start
    INTO #ducklake_assigned_row_id
    FROM #ducklake_staged_data_file s
    LEFT JOIN {SCHEMA}.ducklake_table_stats ts ON ts.table_id = s.table_id;

    INSERT INTO {SCHEMA}.ducklake_data_file
        (data_file_id, table_id, begin_snapshot, end_snapshot, file_order, path, path_is_relative,
         file_format, record_count, file_size_bytes, footer_size, row_id_start, partition_id,
         encryption_key, mapping_id, partial_max)
    SELECT f.data_file_id, s.table_id, @snapshot_id, NULL, s.file_order, s.path, s.path_is_relative,
           s.file_format, s.record_count, s.file_size_bytes, s.footer_size, r.row_id_start,
           s.partition_id, s.encryption_key, s.mapping_id, s.partial_max
    FROM #ducklake_staged_data_file s
    JOIN #ducklake_commit_files f ON f.local_id = s.data_file_id
    JOIN #ducklake_assigned_row_id r ON r.local_id = s.data_file_id;

    -- A partitioned table's files carry their partition values. The staged table is bulk-loaded only
    -- when it has rows - loading an empty one would cost a round trip on every commit of an
    -- unpartitioned table, and round trips are the whole point here - so this reads it through
    -- sp_executesql, which compiles the statement only if the table is actually there. That is also
    -- why the file ids above live in a #temp table rather than a table variable: a table variable is
    -- not visible inside the dynamic statement, a session temp table is.
    IF OBJECT_ID('tempdb..#ducklake_staged_data_file_partition') IS NOT NULL
        EXEC sp_executesql N'
            INSERT INTO {SCHEMA}.ducklake_file_partition_value
                (data_file_id, table_id, partition_key_index, partition_value)
            SELECT f.data_file_id, f.table_id, s.partition_column_idx, s.partition_value
            FROM #ducklake_staged_data_file_partition s
            JOIN #ducklake_commit_files f ON f.local_id = s.local_file_id;';

    INSERT INTO {SCHEMA}.ducklake_file_column_stats
        (data_file_id, table_id, column_id, column_size_bytes, value_count, null_count, min_value,
         max_value, contains_nan, extra_stats)
    SELECT f.data_file_id, s.table_id, s.column_id, s.column_size_bytes,
           CASE WHEN s.has_num_values = 1 THEN s.num_values END,
           CASE WHEN s.has_null_count = 1 THEN s.null_count END,
           CASE WHEN s.has_min = 1 THEN s.min_value END,
           CASE WHEN s.has_max = 1 THEN s.max_value END,
           CASE WHEN s.has_contains_nan = 1 THEN s.contains_nan END,
           s.extra_stats
    FROM #ducklake_staged_data_file_column_stats s
    JOIN #ducklake_commit_files f ON f.local_id = s.data_file_id;

    -- Table totals: a table this commit is the first to write gets a row, the rest are added to.
    -- next_row_id is monotonic - carried forward and advanced by what was inserted, never
    -- recomputed from the files present (DuckLakeTableStats::MergeFileStats). A file that only
    -- rewrites inlined rows into parquet (partial_max set) adds bytes but neither records nor ids.
    MERGE {SCHEMA}.ducklake_table_stats AS t
    USING (
        SELECT table_id,
               SUM(CASE WHEN partial_max IS NULL THEN record_count ELSE 0 END) AS added_records,
               SUM(file_size_bytes) AS added_bytes
        FROM #ducklake_staged_data_file
        GROUP BY table_id
    ) AS s ON t.table_id = s.table_id
    WHEN MATCHED THEN UPDATE SET
        record_count = t.record_count + s.added_records,
        file_size_bytes = t.file_size_bytes + s.added_bytes,
        next_row_id = t.next_row_id + s.added_records
    WHEN NOT MATCHED THEN INSERT (table_id, record_count, next_row_id, file_size_bytes)
        VALUES (s.table_id, s.added_records, s.added_records, s.added_bytes);

    -- Per-column totals: widen the range, and remember a null or a NaN once one appears.
    MERGE {SCHEMA}.ducklake_table_column_stats AS t
    USING (
        SELECT table_id, column_id,
               MAX(CASE WHEN has_null_count = 1 AND null_count > 0 THEN 1 ELSE 0 END) AS any_null,
               MAX(CASE WHEN has_contains_nan = 1 AND contains_nan = 1 THEN 1 ELSE 0 END) AS any_nan,
               MIN(CASE WHEN has_min = 1 THEN min_value END) AS min_value,
               MAX(CASE WHEN has_max = 1 THEN max_value END) AS max_value
        FROM #ducklake_staged_data_file_column_stats
        GROUP BY table_id, column_id
    ) AS s ON t.table_id = s.table_id AND t.column_id = s.column_id
    WHEN MATCHED THEN UPDATE SET
        contains_null = CASE WHEN t.contains_null = 1 OR s.any_null = 1 THEN 1 ELSE t.contains_null END,
        contains_nan = CASE WHEN t.contains_nan = 1 OR s.any_nan = 1 THEN 1 ELSE t.contains_nan END,
        min_value = CASE WHEN t.min_value IS NULL OR s.min_value COLLATE {COLLATION} < t.min_value COLLATE {COLLATION}
                         THEN s.min_value ELSE t.min_value END,
        max_value = CASE WHEN t.max_value IS NULL OR s.max_value COLLATE {COLLATION} > t.max_value COLLATE {COLLATION}
                         THEN s.max_value ELSE t.max_value END
    WHEN NOT MATCHED THEN INSERT (table_id, column_id, contains_null, contains_nan, min_value, max_value, extra_stats)
        VALUES (s.table_id, s.column_id, s.any_null, s.any_nan, s.min_value, s.max_value, NULL);

    DECLARE @added_files BIGINT = (SELECT COUNT(*) FROM #ducklake_staged_data_file);
    INSERT INTO {SCHEMA}.ducklake_snapshot (snapshot_id, snapshot_time, schema_version, next_catalog_id, next_file_id)
    VALUES (@snapshot_id, SYSDATETIMEOFFSET(), @schema_ver, @next_catalog_id, @next_file_id + @added_files);

    DECLARE @changes NVARCHAR(MAX) = (
        SELECT STRING_AGG(CAST('inserted_into_table:' + CAST(table_id AS NVARCHAR(20)) AS NVARCHAR(MAX)), ',')
        FROM (SELECT DISTINCT table_id FROM #ducklake_staged_data_file) d);
    INSERT INTO {SCHEMA}.ducklake_snapshot_changes (snapshot_id, changes_made, author, commit_message, commit_extra_info)
    VALUES (@snapshot_id, @changes, @author, @commit_message, @commit_extra_info);

    -- mssql_exec runs a batch and returns a row count, not a result set, so the values go into the
    -- table this batch created above and are read back from it on the same connection.
    INSERT INTO #ducklake_commit_result (snapshot_id, schema_version, had_flushes)
    VALUES (@snapshot_id, @schema_ver, 0);
)";
	sql = StringUtil::Replace(sql, "{SCHEMA}", schema);
	sql = StringUtil::Replace(sql, "{COLLATION}", collation);
	sql = StringUtil::Replace(sql, "{SCHEMA_VERSION}", to_string(schema_version));
	sql = StringUtil::Replace(sql, "{AUTHOR}", TSQLLiteral(author));
	sql = StringUtil::Replace(sql, "{MESSAGE}", TSQLLiteral(commit_message));
	sql = StringUtil::Replace(sql, "{EXTRA}", TSQLLiteral(commit_extra_info));
	return sql;
}

idx_t MSSQLMetadataManager::StageCommitLocally(DuckLakeTransaction &flush_transaction, const DuckLakeSnapshot &snapshot,
                                               const DuckLakeRetryConfig &retry_config) {
	// The local half: DuckLake's own staging into duckdb TEMPORARY tables, and a count of what
	// landed. Nothing crosses the wire here, which is the point - the decision about whether the
	// server-side apply is worth its bulk loads can be taken before paying for any of them.
	DuckLakeStagedCommit staged;
	auto batch = staged.Build(flush_transaction, snapshot, retry_config);
	auto call = batch.rfind("SELECT * FROM ducklake_commit(");
	if (call == string::npos) {
		throw InternalException("MSSQLMetadataManager: ducklake's staged commit no longer ends with its own call");
	}
	auto staging_sql = batch.substr(0, call);

	auto &connection = flush_transaction.GetConnection();
	auto result = connection.Query(staging_sql);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to stage the DuckLake commit: ");
	}
	auto rows = connection.Query(StringUtil::Format(
	    "SELECT count(*) FROM %s", SQLIdentifier(DuckLakeStagedTable::BaseName(DuckLakeStagedTableType::DATA_FILE))));
	if (rows->HasError()) {
		rows->GetErrorObject().Throw("Failed to inspect the staged DuckLake commit: ");
	}
	auto chunk = rows->Fetch();
	if (!chunk || chunk->size() == 0) {
		return 0;
	}
	return static_cast<idx_t>(chunk->GetValue(0, 0).GetValue<int64_t>());
}

void MSSQLMetadataManager::StageCommit(DuckLakeTransaction &flush_transaction) {
	// The half that crosses the wire. The rows are already staged locally by StageCommitLocally;
	// this bulk-loads each non-empty table into a `#temp` on the transaction's own connection, which
	// is where the apply batch will read them from.
	auto &connection = flush_transaction.GetConnection();
	const auto catalog_name = flush_transaction.GetCatalog().MetadataDatabaseName();
	for (auto type : DuckLakeStagedTable::AllTypes()) {
		const string name = DuckLakeStagedTable::BaseName(type);
		// most of the seventeen are empty in any one commit; a bulk load of nothing is still a round
		// trip, and the count is local
		auto rows = connection.Query(StringUtil::Format("SELECT count(*) FROM %s", SQLIdentifier(name)));
		if (rows->HasError()) {
			rows->GetErrorObject().Throw("Failed to inspect the staged DuckLake commit: ");
		}
		auto chunk = rows->Fetch();
		if (!chunk || chunk->size() == 0 || chunk->GetValue(0, 0).GetValue<int64_t>() == 0) {
			continue;
		}
		// The caller has already established that this commit is data files alone; anything else here
		// means that reading of the transaction disagrees with what DuckLake actually staged, and
		// applying would silently drop it. There is no falling back at this point either - the
		// snapshot fetch may have been skipped on the strength of the same decision.
		if (type != DuckLakeStagedTableType::COMMIT_HEADER && type != DuckLakeStagedTableType::DATA_FILE &&
		    type != DuckLakeStagedTableType::DATA_FILE_COLUMN_STATS &&
		    type != DuckLakeStagedTableType::DATA_FILE_PARTITION) {
			throw InternalException(
			    "MSSQLMetadataManager: the commit was taken for data files alone, but DuckLake staged rows in %s",
			    name);
		}
		// `#name` is a session temp table: private to this connection, and gone if the transaction
		// rolls back. REPLACE, because the connection may have staged an earlier commit already.
		auto copy = connection.Query(
		    StringUtil::Format("COPY %s TO 'mssql://%s/#%s' (FORMAT 'bcp', CREATE_TABLE true, REPLACE true)",
		                       SQLIdentifier(name), catalog_name, name));
		if (copy->HasError()) {
			copy->GetErrorObject().Throw("Failed to bulk-load the staged DuckLake commit: ");
		}
	}
}

namespace {

//! quack's definition: a commit that touches only data, so the server can apply it without any of
//! the catalog's schema bookkeeping (specs/005 D4).
bool IsDataOnlyCommit(const TransactionChangeInformation &c) {
	return c.created_schemas.empty() && c.dropped_schemas.empty() && c.created_tables.empty() &&
	       c.created_scalar_macros.empty() && c.created_table_macros.empty() && c.altered_tables.empty() &&
	       c.altered_tables_with_schema_version_changes.empty() && c.altered_views.empty() &&
	       c.dropped_tables.empty() && c.dropped_views.empty() && c.dropped_scalar_macros.empty() &&
	       c.dropped_table_macros.empty();
}

//! The scope of OUR apply, which is narrower than quack's: data files and nothing else. Decided
//! from the transaction's own change sets, which name every shape separately - so the decision is
//! made BEFORE staging rather than after it.
//!
//! It used to be made after, by staging everything and inspecting what landed. That made a commit
//! the apply does not cover pay for both paths: the full staging, its bulk loads to the server, and
//! then the entire client commit loop from scratch. It was the most expensive shape in the
//! benchmark - 3.5x phase 1 - precisely because it did the work twice (specs/005 D7).
bool IsDataFilesOnlyCommit(const TransactionChangeInformation &c) {
	return IsDataOnlyCommit(c) && !c.tables_inserted_into.empty() && c.tables_deleted_from.empty() &&
	       c.tables_inserted_inlined.empty() && c.tables_deleted_inlined.empty() && c.tables_flushed_inlined.empty() &&
	       c.tables_compacted.empty() && c.tables_merge_adjacent.empty() && c.tables_rewrite_delete.empty();
}

} // namespace

//! The two switches phase 2 is behind while it is measured. Read once - getenv on every commit
//! would be a syscall in the hot path.
static bool ServerCommitEnabled() {
	static const bool enabled = getenv("MSSQL_DUCKLAKE_SERVER_COMMIT") != nullptr;
	return enabled;
}

//! The commit size at which the server-side apply starts paying for its bulk loads. Measured at
//! about sixteen data files (specs/005 D7); a setting because the crossover moves with latency, and
//! on a link slower than a loopback socket it moves down.
static idx_t ServerCommitMinFiles() {
	static const idx_t threshold = []() -> idx_t {
		auto *env = getenv("MSSQL_DUCKLAKE_SERVER_COMMIT_MIN_FILES");
		if (!env) {
			return 16;
		}
		try {
			auto value = std::stoll(env);
			return value < 0 ? 0 : static_cast<idx_t>(value);
		} catch (const std::exception &) {
			return 16;
		}
	}();
	return threshold;
}

static bool SkipSnapshotFetchEnabled() {
	static const bool enabled = getenv("MSSQL_DUCKLAKE_SERVER_COMMIT_SKIP_FETCH") != nullptr;
	return enabled;
}

unique_ptr<QueryResult> MSSQLMetadataManager::Query(DuckLakeSnapshot snapshot, string &query) {
	// Recognised by comparing with DuckLake's own template, before any placeholder is substituted -
	// which is why this overload and not Query(string &): here the text is still the raw template,
	// and {SNAPSHOT_ID} has not yet been replaced with a number that would defeat the comparison.
	// Nothing is parsed out of the query: the catalog and schema the replacement needs are the
	// manager's own, and they arrive through the same {METADATA_CATALOG} substitution the base
	// applies next.
	//
	// An exact match, so a ducklake bump that edits the query stops matching rather than applying a
	// rewrite to something that no longer says what we think. ProbeServerCapabilities turns that
	// mismatch into an error at attach, because the alternative is a silent return of the crash.
	//
	// The switch exists so the regression test can be shown to fail without the rewrite - a test that
	// cannot fail proves nothing, and this one did pass without it until its sensitivity was checked.
	// It is asked only after the query has already matched, which is once per commit retry rather
	// than on every metadata query this override sees.
	if (query == DUCKLAKE_CONFLICT_CHECK_QUERY && getenv("MSSQL_DUCKLAKE_NO_CONFLICT_REWRITE") == nullptr) {
		query = MSSQL_CONFLICT_CHECK_QUERY;
	}
	return DuckLakeMetadataManager::Query(snapshot, query);
}

string MSSQLMetadataManager::GetLatestSnapshotQuery() const {
	// Read through `mssql_scan` instead of through the attached catalog, which is what the postgres
	// manager does with this same query. Measured (specs/005 D13), the same rows cost 10.0ms through
	// DuckDB's catalog against 2.5ms as a direct scan: binding, the catalog entry lookup and
	// materialising the scan inside the transaction, none of which the direct path pays. It is not
	// the wire - a trivial round trip here is 1.0ms against postgres's 2.3ms.
	//
	// Only a query shaped like this one can take the direct path on v0.2.5. The extension
	// materialises a catalog scan but not a plan holding two `mssql_scan`s inside a transaction, so
	// the direct form is available exactly where the scan is the SOLE source of its query. This one
	// is; the file-column-stats CTE is not, and waits for the duckdb 2.0 line.
	//
	// TOP 1 descending rather than the base's MAX subquery: same row, one seek down the primary key
	// this manager puts on ducklake_snapshot, and no self-join for the server to unpick.
	return R"(SELECT * FROM mssql_scan({METADATA_CATALOG_NAME_LITERAL}, 'SELECT TOP 1 snapshot_id, )"
	       R"(schema_version, next_catalog_id, next_file_id FROM {METADATA_SCHEMA_ESCAPED}.ducklake_snapshot )"
	       R"(ORDER BY snapshot_id DESC'))";
}

bool MSSQLMetadataManager::CanSkipSnapshotFetch(const TransactionChangeInformation &changes) const {
	// Measured (specs/005 D7), the apply saves little on round trips by itself - 15.2 against the
	// client loop's 16.0 per commit - because phase 1's Execute passthrough already sends the commit
	// as few batches. Skipping the fetch was meant to be the rest of the saving; measured, it is not
	// (D7 again), which is why this stays behind its own switch rather than being armed with the
	// apply.
	//
	// The invariant that makes it safe to answer yes: DuckLake holds `snapshot_lock` across the call
	// it makes when this returns true, and GetSnapshot() takes that same non-recursive mutex - so a
	// commit that skipped the fetch can never ask for the snapshot afterwards. This must therefore
	// answer for EXACTLY the commits FlushChangesServerSide applies without falling back, which is
	// why both ask IsDataFilesOnlyCommit and neither decides anything after staging.
	return SkipSnapshotFetchEnabled() && ServerCommitEnabled() && !transaction.GetRequiresNewInlinedTable() &&
	       IsDataFilesOnlyCommit(changes);
}

void MSSQLMetadataManager::FlushChangesServerSide(DuckLakeTransaction &flush_transaction,
                                                  DuckLakeSnapshot transaction_snapshot,
                                                  const TransactionChangeInformation &transaction_changes,
                                                  const DuckLakeRetryConfig &retry_config) {
	// Decided here, BEFORE anything is staged. Staging a commit the apply cannot finish means paying
	// for both paths - the staging, its bulk loads, and then the whole client loop from scratch -
	// which was the worst shape in the benchmark (specs/005 D7). This is also exactly what
	// CanSkipSnapshotFetch answers, which is what makes skipping the fetch safe; see there.
	if (!IsDataFilesOnlyCommit(transaction_changes) || flush_transaction.GetRequiresNewInlinedTable()) {
		flush_transaction.RunCommitLoop(transaction_snapshot, transaction_changes, retry_config);
		return;
	}

	// Stage locally first and count what came out. The apply is worth its bulk loads only above a
	// size: measured per commit, phase 2 is 1.34x the client loop at one data file and 0.40x at 256,
	// crossing over at about sixteen (specs/005 D5, D7). Below that the loop is simply the cheaper
	// answer, and this is the only point at which that can still be chosen - nothing has crossed the
	// wire yet.
	auto staged_files = StageCommitLocally(flush_transaction, transaction_snapshot, retry_config);
	if (staged_files < ServerCommitMinFiles() && !SkipSnapshotFetchEnabled()) {
		// Local staging leaves the transaction untouched, so the loop reads exactly what it would
		// have read. (With the snapshot fetch skipped there is no falling back - see
		// CanSkipSnapshotFetch - so that switch takes the apply whatever the size.)
		flush_transaction.RunCommitLoop(transaction_snapshot, transaction_changes, retry_config);
		return;
	}
	StageCommit(flush_transaction);

	auto &commit_info = flush_transaction.GetCommitInfo();
	auto &connection = flush_transaction.GetConnection();
	const int64_t schema_version = transaction_snapshot.snapshot_id != DConstants::INVALID_INDEX
	                                   ? static_cast<int64_t>(transaction_snapshot.schema_version)
	                                   : -1;
	// One batch, which also creates the table it reports through: mssql_exec returns a row count
	// rather than a result set, and the values are read back from that table afterwards.
	auto call = StringUtil::Format(
	    "SELECT mssql_exec(%s, %s)", CatalogLiteral(),
	    SQLString(
	        CommitBatchSql(SchemaIdentifier(), VARCHAR_COLLATION, schema_version,
	                       commit_info.author.IsNull() ? "" : commit_info.author.ToString(),
	                       commit_info.commit_message.IsNull() ? "" : commit_info.commit_message.ToString(),
	                       commit_info.commit_extra_info.IsNull() ? "" : commit_info.commit_extra_info.ToString())));
	auto applied = connection.Query(call);
	// No fallback from here on: the procedure writes inside this transaction, so the client loop
	// cannot start over in it. Everything that chooses between the two paths happens before the call.
	if (applied->HasError()) {
		applied->GetErrorObject().Throw("The server-side DuckLake commit failed: ");
	}
	auto result = connection.Query(
	    StringUtil::Format("SELECT snapshot_id, schema_version, had_flushes FROM mssql_scan(%s, 'SELECT snapshot_id, "
	                       "schema_version, had_flushes FROM #ducklake_commit_result')",
	                       CatalogLiteral()));
	if (result->HasError()) {
		result->GetErrorObject().Throw("The server-side DuckLake commit did not report its snapshot: ");
	}
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() == 0) {
		throw IOException("The server-side DuckLake commit returned no snapshot");
	}
	// A NULL here means the batch ran but decided nothing - most likely it found no snapshot to build
	// on. Say so, rather than letting duckdb raise an internal error out of a NULL Value.
	if (chunk->GetValue(0, 0).IsNull() || chunk->GetValue(1, 0).IsNull()) {
		throw IOException("The server-side DuckLake commit reported no snapshot (snapshot_id=%s, schema_version=%s)",
		                  chunk->GetValue(0, 0).ToString(), chunk->GetValue(1, 0).ToString());
	}
	auto committed_snapshot_id = chunk->GetValue(0, 0).GetValue<int64_t>();
	auto committed_schema_version = chunk->GetValue(1, 0).GetValue<int64_t>();
	auto had_flushes = !chunk->GetValue(2, 0).IsNull() && chunk->GetValue(2, 0).GetValue<bool>();
	flush_transaction.GetCatalog().SetCommittedSnapshotId(static_cast<idx_t>(committed_snapshot_id));
	flush_transaction.ApplyServerSideCommit(static_cast<idx_t>(committed_schema_version));
	if (had_flushes) {
		flush_transaction.DropEmptySupersededInlinedTablesClientSide();
		// quack clears unconditionally, because its apply can create inlined-data tables and the mssql
		// extension would not see them. Ours creates no table at all - it writes rows into catalog
		// tables that already exist - and clearing costs the whole schema's metadata: 39 introspection
		// round trips on the next access, measured, against the ~20 the commit itself needs. So it is
		// cleared only when the server actually flushed something (specs/005 D7).
		ClearCache();
	}
}

void MSSQLMetadataManager::ProbeServerCapabilities() {
	// Runs once per attach, including for a catalog this manager did not create - one made by an
	// older build, or by a run that failed between DuckLake's DDL and ours. Without the keys such a
	// catalog is readable but not writable, and the failure would surface much later as "requires a
	// table with a primary key". Applying the DDL unconditionally cost two seconds on every attach
	// (measured against the postgres backend), so ask first: one round trip when the catalog is
	// already in shape, which is every attach after the first.
	if (!CatalogShapeIsCurrent()) {
		EnsureCatalogShape();
	}
	// Phase 2 is off by default because it is incomplete, not because it is wrong: the apply is
	// correct for the commits it accepts - it produces a catalog identical to the client loop's -
	// but it accepts only data files (specs/005 D4), and below a threshold the staging costs more
	// than the loop it would replace (D5). MSSQL_DUCKLAKE_SERVER_COMMIT=1 turns it on for that work.
	if (ServerCommitEnabled()) {
		transaction.GetCatalog().SetRetrialsServerSide(true);
	}
	// The conflict-check rewrite (specs/007 D1) recognises DuckLake's query by an exact match. If a
	// ducklake bump edits that query the match simply stops happening, the base query runs, and the
	// concurrent-commit crash comes back - silently, in a path only concurrent writers reach. So the
	// mismatch is made loud here instead: one string comparison per attach, and a bump that touches
	// the query fails the integration suite rather than shipping a correctness regression.
	if (DuckLakeMetadataManager::GetSnapshotAndStatsAndChangesQuery() != DUCKLAKE_CONFLICT_CHECK_QUERY) {
		throw InvalidInputException(
		    "mssql_ducklake: DuckLake's conflict-check query has changed in this ducklake pin. This "
		    "manager rewrites that query so it reads ducklake_snapshot once instead of twice "
		    "(specs/007); re-audit the rewrite against the new text and update "
		    "DUCKLAKE_CONFLICT_CHECK_QUERY.");
	}
}

//===--------------------------------------------------------------------===//
// The inlined data table (specs/004 D2)
//===--------------------------------------------------------------------===//

string MSSQLMetadataManager::GetInlinedTableQueries(DuckLakeSnapshot commit_snapshot, const DuckLakeTableInfo &table,
                                                    string &inlined_tables, string &inlined_table_queries) {
	// The base registers the table and builds DuckDB DDL for it; we keep the registration and write
	// the DDL ourselves, because the column types are T-SQL and could not travel in the duckdb batch.
	string base_ddl;
	auto table_name = DuckLakeMetadataManager::GetInlinedTableQueries(commit_snapshot, table, inlined_tables, base_ddl);
	if (base_ddl.empty()) {
		return table_name;
	}

	string columns;
	for (auto &column : table.columns) {
		columns += StringUtil::Format(", %s %s", SQLIdentifier(column.name),
		                              TSQLColumnType(DuckLakeTypes::FromString(column.type)));
	}
	// An inlined row is updated (its end_snapshot is set) and deleted, so this table needs a key for
	// the same reason the catalog's own tables do.
	auto statement = StringUtil::Format(
	    "IF OBJECT_ID(QUOTENAME(%s) + '.' + QUOTENAME(%s)) IS NULL "
	    "CREATE TABLE %s.%s(row_id BIGINT NOT NULL, begin_snapshot BIGINT NOT NULL, end_snapshot BIGINT%s, "
	    "CONSTRAINT %s PRIMARY KEY (row_id, begin_snapshot));",
	    DuckLakeUtil::SQLLiteralToString(transaction.GetCatalog().MetadataSchemaName()),
	    DuckLakeUtil::SQLLiteralToString(table_name), SchemaIdentifier(), SQLIdentifier(table_name), columns,
	    SQLIdentifier("pk_" + table_name));
	RunServerSideOutsideTransaction(statement, "Failed to create the inlined data table: ");
	// Refresh the extension's view of THIS table now, before returning into the batch being built.
	// DuckLake clears the cache only after the commit batch has run - CommitChanges builds it (and
	// gets here), execute_commit_batch runs it, and flush_cache_if_pending comes after that - so a
	// table created and written in the SAME commit is invisible to the very INSERT that needs it and
	// the commit dies with "Table with name ducklake_inlined_data_<table>_<version> does not exist".
	// Creating a table and inlining rows into it in one statement is the ordinary case: any
	// CREATE TABLE ... AS SELECT under the inlining limit. The base's deletion-table path does not
	// have the problem because it invalidates there itself, right after creating the table.
	//
	// Safe here where a schema-wide clear at this point is not: the table was created OUTSIDE this
	// transaction (see RunServerSideOutsideTransaction), so it is committed and holds no lock that
	// the extension's metadata read - which takes its own connection - could wait on.
	InvalidateTableCache(table_name);
	// Recorded as well, so DuckLake's own clear after the batch stays the cheap targeted one rather
	// than falling back to re-reading the whole schema. The repeat costs a single round trip.
	tables_pending_cache_refresh.push_back(table_name);
	return table_name;
}

} // namespace duckdb
