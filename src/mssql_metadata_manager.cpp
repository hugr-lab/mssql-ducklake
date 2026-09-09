#include "mssql_metadata_manager.hpp"

#include "common/ducklake_types.hpp"
#include "common/ducklake_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/main/database.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_metadata_info.hpp"
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
