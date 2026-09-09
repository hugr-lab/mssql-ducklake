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
constexpr const char *MSSQLMetadataManager::SHAPE_MARKER_CONSTRAINT;

MSSQLMetadataManager::MSSQLMetadataManager(DuckLakeTransaction &transaction) : DuckLakeMetadataManager(transaction) {
}

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
	// on the schema locks this transaction holds. Scoped to our schema rather than the whole
	// catalog, which would drop every table's metadata on every inlined-table creation.
	auto &connection = transaction.GetConnection();
	auto schema = DuckLakeUtil::SQLLiteralToString(transaction.GetCatalog().MetadataSchemaName());
	auto result =
	    connection.Query(StringUtil::Format("SELECT mssql_invalidate_cache(%s, %s)", CatalogLiteral(), schema));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to refresh the SQL Server catalog cache: ");
	}
}

//===--------------------------------------------------------------------===//
// Initialization (specs/004 D3)
//===--------------------------------------------------------------------===//

bool MSSQLMetadataManager::CatalogShapeIsCurrent() {
	// One cheap question instead of two batches of DDL: the last constraint the shaping applies is
	// the marker for all of it. Attaching an existing catalog is on the hot path - every transaction
	// pays for whatever happens here - and the DDL is only ever needed once per catalog.
	auto &connection = transaction.GetConnection();
	// the inner statement travels inside a duckdb string literal, so each of its own quotes is
	// doubled once - the same shape the collation probe above uses
	auto schema_name = StringUtil::Replace(transaction.GetCatalog().MetadataSchemaName(), "'", "''''");
	auto result = connection.Query(
	    StringUtil::Format("SELECT keys FROM mssql_scan(%s, 'SELECT COUNT(*) AS keys FROM sys.key_constraints "
	                       "WHERE name = ''%s'' AND schema_id = SCHEMA_ID(''%s'')')",
	                       CatalogLiteral(), SHAPE_MARKER_CONSTRAINT, schema_name));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to inspect the DuckLake catalog on SQL Server: ");
	}
	auto row = result->Fetch();
	return row && row->size() > 0 && !row->GetValue(0, 0).IsNull() && row->GetValue(0, 0).GetValue<int64_t>() > 0;
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

	// Filtered indexes on the condition almost every DuckLake read carries. Neither the postgres nor
	// the sqlite manager has indexes here at all.
	const vector<pair<string, string>> live_indexes = {
	    {"ducklake_data_file", "table_id"}, {"ducklake_delete_file", "table_id"}, {"ducklake_table", "schema_id"},
	    {"ducklake_column", "table_id"},    {"ducklake_view", "schema_id"},
	};
	for (auto &entry : live_indexes) {
		constraints_ddl += StringUtil::Format("IF NOT EXISTS (SELECT 1 FROM sys.indexes WHERE name = 'ix_%s_live') "
		                                      "CREATE INDEX ix_%s_live ON %s.%s(%s) WHERE end_snapshot IS NULL;\n",
		                                      entry.first, entry.first, schema, entry.first, entry.second);
	}

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
    DECLARE @files TABLE (local_id BIGINT PRIMARY KEY, data_file_id BIGINT, table_id BIGINT);
    INSERT INTO @files (local_id, data_file_id, table_id)
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
    JOIN @files f ON f.local_id = s.data_file_id
    JOIN #ducklake_assigned_row_id r ON r.local_id = s.data_file_id;

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
    JOIN @files f ON f.local_id = s.data_file_id;

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

MSSQLMetadataManager::StagedCommit MSSQLMetadataManager::StageCommit(DuckLakeTransaction &flush_transaction,
                                                                     const DuckLakeSnapshot &snapshot,
                                                                     const DuckLakeRetryConfig &retry_config) {
	// DuckLake already knows how to turn a transaction into staged rows - seventeen flat tables of
	// scalars - and doing that ourselves would be a copy to re-audit on every submodule bump. Its
	// batch ends with the call to its own ducklake_commit; we keep the staging half and supply the
	// call ourselves, in T-SQL.
	DuckLakeStagedCommit staged;
	auto batch = staged.Build(flush_transaction, snapshot, retry_config);
	auto call = batch.rfind("SELECT * FROM ducklake_commit(");
	if (call == string::npos) {
		throw InternalException("MSSQLMetadataManager: ducklake's staged commit no longer ends with its own call");
	}
	auto staging_sql = batch.substr(0, call);

	// The staging tables are duckdb TEMPORARY tables, so they live on the connection that creates
	// them - which has to be the one the bulk load reads from, and the one holding the transaction.
	auto &connection = flush_transaction.GetConnection();
	auto result = connection.Query(staging_sql);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to stage the DuckLake commit: ");
	}

	StagedCommit report;
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
		// what the server-side apply understands so far, and everything else is a reason to fall back
		if (type != DuckLakeStagedTableType::COMMIT_HEADER && type != DuckLakeStagedTableType::DATA_FILE &&
		    type != DuckLakeStagedTableType::DATA_FILE_COLUMN_STATS) {
			report.only_data_files = false;
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
	return report;
}

namespace {

//! quack's definition, and the scope of the fast path: a commit that touches only data, so the
//! server can apply it without any of the catalog's schema bookkeeping (specs/005 D4).
bool IsDataOnlyCommit(const TransactionChangeInformation &c) {
	return c.created_schemas.empty() && c.dropped_schemas.empty() && c.created_tables.empty() &&
	       c.created_scalar_macros.empty() && c.created_table_macros.empty() && c.altered_tables.empty() &&
	       c.altered_tables_with_schema_version_changes.empty() && c.altered_views.empty() &&
	       c.dropped_tables.empty() && c.dropped_views.empty() && c.dropped_scalar_macros.empty() &&
	       c.dropped_table_macros.empty();
}

} // namespace

bool MSSQLMetadataManager::CanSkipSnapshotFetch(const TransactionChangeInformation &changes) const {
	// Not yet: the apply covers only data-file commits (specs/005 D4), so the client still has to
	// fetch its own snapshot for everything else. Skipping the fetch is part of arming phase 2.
	return false;
}

void MSSQLMetadataManager::FlushChangesServerSide(DuckLakeTransaction &flush_transaction,
                                                  DuckLakeSnapshot transaction_snapshot,
                                                  const TransactionChangeInformation &transaction_changes,
                                                  const DuckLakeRetryConfig &retry_config) {
	if (!IsDataOnlyCommit(transaction_changes) || flush_transaction.GetRequiresNewInlinedTable()) {
		flush_transaction.RunCommitLoop(transaction_snapshot, transaction_changes, retry_config);
		return;
	}
	auto staged = StageCommit(flush_transaction, transaction_snapshot, retry_config);
	if (!staged.only_data_files) {
		// The procedure covers data files and nothing else yet (specs/005 D3). Anything else staged -
		// a delete file, inlined rows, a compaction - takes the client loop, which reads the same
		// transaction state and is unaffected by the staging that just happened.
		flush_transaction.RunCommitLoop(transaction_snapshot, transaction_changes, retry_config);
		return;
	}

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
	}
	// the same two calls quack makes after a server-side commit: the catalog cache cannot have seen
	// what the server just wrote
	ClearCache();
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
	if (getenv("MSSQL_DUCKLAKE_SERVER_COMMIT")) {
		transaction.GetCatalog().SetRetrialsServerSide(true);
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
	return table_name;
}

} // namespace duckdb
