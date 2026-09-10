#include "mssql_metadata_manager.hpp"
#include "mssql_metadata_internal.hpp"

#include "common/ducklake_types.hpp"
#include "common/ducklake_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/logging/logger.hpp"
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
	if (!InlinedDeletionDdlIsDuckLakes()) {
		throw InvalidInputException(
		    "mssql_ducklake: DuckLake's DDL for a new inlined deletion table has changed in this ducklake "
		    "pin. The commit batch seam creates that table keyed in its place (specs/006 D5b); re-audit "
		    "the text and update INLINED_DELETE_DDL_HEAD/TAIL.");
	}
	if (!ConflictCheckQueryIsDuckLakes()) {
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
