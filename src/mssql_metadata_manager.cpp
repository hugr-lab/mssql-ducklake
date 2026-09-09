#include "mssql_metadata_manager.hpp"

#include "common/ducklake_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/main/database.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

MSSQLMetadataManager::MSSQLMetadataManager(DuckLakeTransaction &transaction) : DuckLakeMetadataManager(transaction) {
}

bool MSSQLMetadataManager::TypeIsNativelySupported(const LogicalType &type) {
	switch (type.id()) {
	// SQL Server has no NaN and no infinities, so a float column cannot round trip
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	// DATETIME2(7) is 100ns, so nanoseconds are lossy
	case LogicalTypeId::TIMESTAMP_NS:
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

string MSSQLMetadataManager::GetColumnTypeInternal(const LogicalType &column_type) {
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
		// Everything else - the types above that are not natively supported, and DuckLake's own
		// text storage for nested values. MAX rather than a bound: DuckLake states no length, and a
		// bare VARCHAR means VARCHAR(1) in T-SQL. The collation is explicit rather than inherited
		// from the database, whose own is often a legacy CI_AS one.
		return StringUtil::Format("VARCHAR(MAX) COLLATE %s", VARCHAR_COLLATION);
	}
}

string MSSQLMetadataManager::SchemaIdentifier() const {
	return DuckLakeUtil::SQLIdentifierToString(transaction.GetCatalog().MetadataSchemaName());
}

string MSSQLMetadataManager::CatalogLiteral() const {
	return DuckLakeUtil::SQLLiteralToString(transaction.GetCatalog().MetadataDatabaseName());
}

void MSSQLMetadataManager::RunServerSide(const string &tsql, const string &context) {
	auto &connection = transaction.GetConnection();
	auto result = connection.Query(StringUtil::Format("SELECT mssql_exec(%s, %s)", CatalogLiteral(), SQLString(tsql)));
	if (result->HasError()) {
		result->GetErrorObject().Throw(context);
	}
}

void MSSQLMetadataManager::RunServerSideOutsideTransaction(const string &tsql, const string &context) {
	// A table this transaction creates but has not committed is locked against the metadata query
	// the mssql extension runs to discover it - and that query takes its own connection, so it waits
	// on us until it times out. Creating the table on a connection of its own, in autocommit, makes
	// it visible immediately and holds no lock. The statement is `IF OBJECT_ID(...) IS NULL`-shaped,
	// so a rolled back commit leaves at worst an empty table that the next attempt reuses.
	auto client_context = transaction.context.lock();
	if (!client_context) {
		throw InternalException("MSSQLMetadataManager: the client context is gone");
	}
	Connection connection(*client_context->db);
	auto result = connection.Query(StringUtil::Format("SELECT mssql_exec(%s, %s)", CatalogLiteral(), SQLString(tsql)));
	if (result->HasError()) {
		result->GetErrorObject().Throw(context);
	}
}

void MSSQLMetadataManager::ClearCache() {
	// The mssql extension caches catalog metadata, and a table created behind its back through
	// mssql_exec is invisible to the duckdb reads that follow - they miss it silently rather than
	// failing. DuckLake already knows when that has happened (`MarkPendingCacheClear` on creating an
	// inlined table) and calls this after the commit, which is the only safe moment: a refresh
	// issued mid-transaction queries the catalog on a second connection and blocks on the schema
	// locks the transaction itself is holding. The extension's own setting for this is global and
	// fires on every DML; this is the point version.
	auto &connection = transaction.GetConnection();
	auto result = connection.Query(StringUtil::Format("SELECT mssql_invalidate_cache(%s)", CatalogLiteral()));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to refresh the SQL Server catalog cache: ");
	}
}

void MSSQLMetadataManager::InitializeDuckLake(bool has_explicit_schema, DuckLakeEncryption encryption) {
	auto &connection = transaction.GetConnection();
	// SQL Server 2019 is the floor: older servers have no UTF-8 collation, so the catalog could not
	// store the strings DuckLake puts in it without lossy conversion. SERVERPROPERTY returns
	// sql_variant, which the mssql extension cannot decode - hence the cast, server-side.
	auto version = connection.Query(StringUtil::Format(
	    "SELECT major FROM mssql_scan(%s, 'SELECT CAST(SERVERPROPERTY(''ProductMajorVersion'') AS INT) AS major')",
	    CatalogLiteral()));
	if (version->HasError()) {
		version->GetErrorObject().Throw("Failed to read the SQL Server version: ");
	}
	auto major = version->Fetch();
	if (!major || major->size() == 0 || major->GetValue(0, 0).IsNull() ||
	    major->GetValue(0, 0).GetValue<int32_t>() < 15) {
		throw NotImplementedException("A DuckLake catalog needs SQL Server 2019 or newer: its string columns are "
		                              "stored with a UTF-8 collation, which older versions do not have.");
	}

	// DuckLake's own DDL first, through duckdb - it owns the shape of its catalog, and reproducing
	// it here would be a copy to re-audit on every submodule bump.
	DuckLakeMetadataManager::InitializeDuckLake(has_explicit_schema, encryption);

	const string schema = SchemaIdentifier();
	// Two phases, because within one T-SQL batch a column's new NOT NULL is not yet visible to a
	// constraint that needs it - the server answers "cannot define PRIMARY KEY on a nullable column".
	string columns_ddl;
	string constraints_ddl;

	// Primary keys. DuckLake declares a few itself; the rest are ours, and they are what make the
	// catalog writable: the mssql extension builds a row identity out of the primary key, and
	// without one it refuses every UPDATE and DELETE - which a commit is full of.
	const vector<pair<string, string>> keys = {
	    {"ducklake_table_stats", "table_id"},
	    {"ducklake_table_column_stats", "table_id, column_id"},
	    {"ducklake_table", "table_id, begin_snapshot"},
	    {"ducklake_view", "view_id, begin_snapshot"},
	    {"ducklake_column", "table_id, column_id, begin_snapshot"},
	    {"ducklake_tag", "object_id, begin_snapshot, [key]"},
	    {"ducklake_column_tag", "table_id, column_id, begin_snapshot, [key]"},
	    {"ducklake_partition_info", "partition_id"},
	    {"ducklake_sort_info", "sort_id"},
	    {"ducklake_macro", "macro_id, begin_snapshot"},
	    {"ducklake_inlined_data_tables", "table_id, schema_version"},
	    {"ducklake_files_scheduled_for_deletion", "data_file_id"},
	};
	for (auto &entry : keys) {
		for (auto &column : StringUtil::Split(entry.second, ',')) {
			auto name = column;
			StringUtil::Trim(name);
			// a key column has to be NOT NULL, and DuckLake declares none of them so. The tag tables
			// key on a name rather than an id; a primary key cannot be over MAX, so it is capped -
			// 200 bytes of UTF-8 is a long tag name and well inside the 900-byte index limit.
			const bool is_name = name == "[key]";
			columns_ddl += StringUtil::Format(
			    "ALTER TABLE %s.%s ALTER COLUMN %s %s NOT NULL;\n", schema, entry.first, name,
			    is_name ? StringUtil::Format("VARCHAR(200) COLLATE %s", VARCHAR_COLLATION) : string("BIGINT"));
		}
		constraints_ddl += StringUtil::Format("ALTER TABLE %s.%s ADD CONSTRAINT pk_%s PRIMARY KEY (%s);\n", schema,
		                                      entry.first, entry.first, entry.second);
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
		constraints_ddl += StringUtil::Format("CREATE INDEX ix_%s_live ON %s.%s(%s) WHERE end_snapshot IS NULL;\n",
		                                      entry.first, schema, entry.first, entry.second);
	}

	RunServerSide(columns_ddl, "Failed to prepare the DuckLake catalog columns for SQL Server: ");
	RunServerSide(constraints_ddl, "Failed to key and index the DuckLake catalog for SQL Server: ");
}

string MSSQLMetadataManager::GetInlinedTableQueries(DuckLakeSnapshot commit_snapshot, const DuckLakeTableInfo &table,
                                                    string &inlined_tables, string &inlined_table_queries) {
	// Let the base build the registration tuple and the DDL, then take the DDL out of the batch and
	// run it ourselves: it carries our column types, which are T-SQL and would not parse in the
	// duckdb batch the rest of the commit travels in.
	string ddl;
	auto table_name = DuckLakeMetadataManager::GetInlinedTableQueries(commit_snapshot, table, inlined_tables, ddl);
	if (!ddl.empty()) {
		auto statement = StringUtil::Replace(ddl, "{METADATA_CATALOG}", SchemaIdentifier());
		// IF NOT EXISTS is DuckDB's spelling; T-SQL asks the question with OBJECT_ID
		statement = StringUtil::Replace(statement, "CREATE TABLE IF NOT EXISTS ", "CREATE TABLE ");
		// An inlined row is updated (its end_snapshot is set) and deleted, so this table needs a key
		// for the same reason the catalog's own tables do - the mssql extension builds a row identity
		// out of it. (row_id, begin_snapshot) is what identifies a version of an inlined row.
		statement = StringUtil::Replace(statement, "row_id BIGINT, begin_snapshot BIGINT,",
		                                "row_id BIGINT NOT NULL, begin_snapshot BIGINT NOT NULL,");
		const auto close_paren = statement.rfind(')');
		if (close_paren != string::npos) {
			statement.insert(close_paren,
			                 StringUtil::Format(", CONSTRAINT pk_%s PRIMARY KEY (row_id, begin_snapshot)", table_name));
		}
		statement = StringUtil::Format("IF OBJECT_ID('%s.%s') IS NULL %s",
		                               transaction.GetCatalog().MetadataSchemaName(), table_name, statement);
		RunServerSideOutsideTransaction(statement, "Failed to create the inlined data table: ");
	}
	(void)inlined_table_queries;
	return table_name;
}

} // namespace duckdb
