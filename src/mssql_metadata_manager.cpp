#include "mssql_metadata_manager.hpp"
#include "mssql_metadata_internal.hpp"
#include "duckdb/storage/object_cache.hpp"

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

bool MSSQLMetadataManager::SupportsAppender() const {
	// The appender writes file statistics as they are, past the commit batch that bounds them: a
	// commit carrying a min or max longer than the catalog's stats_length takes the batch instead,
	// the rest keep the appender (specs/018).
	return !AppenderDisabled() && !HasStatsPastBound();
}

bool MSSQLMetadataManager::HasStatsPastBound() const {
	if (stats_length <= 0) {
		return false;
	}
	auto past = [&](const DuckLakeDataFile &file) {
		for (auto &entry : file.column_stats) {
			auto &stats = entry.second;
			if ((stats.has_min && stats.min.size() > idx_t(stats_length)) ||
			    (stats.has_max && stats.max.size() > idx_t(stats_length))) {
				return true;
			}
		}
		return false;
	};
	for (auto &entry : transaction.GetLocalChanges().Changes()) {
		auto &changes = entry.GetTableChanges();
		for (auto &file : changes.new_data_files) {
			if (past(file)) {
				return true;
			}
		}
		for (auto &compaction : changes.compactions) {
			for (auto &file : compaction.written_files) {
				if (past(file)) {
					return true;
				}
			}
		}
	}
	return false;
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
	return DuckLakeUtil::SQLIdentifierToString(transaction.GetCatalog().MetadataSchemaName().GetIdentifierName());
}

string MSSQLMetadataManager::SchemaLiteral() const {
	return DuckLakeUtil::SQLLiteralToString(transaction.GetCatalog().MetadataSchemaName().GetIdentifierName());
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
	auto schema = DuckLakeUtil::SQLLiteralToString(transaction.GetCatalog().MetadataSchemaName().GetIdentifierName());
	vector<string> calls;
	if (tables_pending_cache_refresh.empty()) {
		calls.push_back(StringUtil::Format("SELECT mssql_invalidate_cache(%s, %s)", CatalogLiteral(), schema));
	} else {
		for (auto &table_name : tables_pending_cache_refresh) {
			if (tables_already_refreshed.count(table_name)) {
				continue;
			}
			calls.push_back(StringUtil::Format("SELECT mssql_invalidate_cache(%s, %s, %s)", CatalogLiteral(), schema,
			                                   DuckLakeUtil::SQLLiteralToString(table_name)));
		}
	}
	tables_pending_cache_refresh.clear();
	tables_already_refreshed.clear();
	for (auto &call : calls) {
		auto result = connection.Query(call);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to refresh the SQL Server catalog cache: ");
		}
	}
}

void MSSQLMetadataManager::InvalidateTableCache(const string &table_name) {
	auto &connection = transaction.GetConnection();
	auto result = connection.Query(StringUtil::Format(
	    "SELECT mssql_invalidate_cache(%s, %s, %s)", CatalogLiteral(),
	    DuckLakeUtil::SQLLiteralToString(transaction.GetCatalog().MetadataSchemaName().GetIdentifierName()),
	    DuckLakeUtil::SQLLiteralToString(table_name)));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to refresh the SQL Server catalog cache: ");
	}
}

//! "This attach of this catalog is ready": one entry per attached database, in the database
//! instance's object cache. The key is the AttachedDatabase's oid, which DuckDB hands out once per
//! ATTACH and never reuses within an instance - so a DETACH and re-ATTACH asks the server again, and
//! the entry dies with the instance (the test runner makes many in one process, and a static would
//! carry a flag from one into the next). Non-evictable; were it evicted, the cost would be one more
//! read of the markers, which are the truth.
class MSSQLCatalogReadyEntry : public ObjectCacheEntry {
public:
	static string ObjectType() {
		return "mssql_ducklake_ready";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx();
	}
	//! the catalog's bound on statistics strings, 0 for MAX (specs/018)
	int64_t stats_length = 0;
};

void MSSQLMetadataManager::ProbeServerCapabilities() {
	// At format 1.0 this runs on our manager once per attach; at 1.1 the attach runs on a stock one
	// and this never reaches us. EnsureReady is the same work for both, on first use.
	EnsureReady();
}

void MSSQLMetadataManager::EnsureReady() {
	if (ready) {
		return;
	}
	auto client_context = transaction.context.lock();
	if (!client_context) {
		throw InternalException("MSSQLMetadataManager: the client context is gone");
	}
	auto &catalog = transaction.GetCatalog();
	// The attach's own first queries run before the catalog knows its metadata schema. Asked then,
	// the markers resolve an empty schema to the login's default one, and a shaping would write
	// `"".ducklake_...` (error 1038). Nothing to decide yet - and not marked, so a later query asks.
	if (catalog.MetadataSchemaName().GetIdentifierName().empty()) {
		return;
	}
	auto &cache = ObjectCache::GetObjectCache(*client_context);
	auto key = StringUtil::Format("mssql_ducklake:ready:%llu", catalog.GetAttached().oid);
	if (auto entry = cache.Get<MSSQLCatalogReadyEntry>(key)) {
		stats_length = entry->stats_length;
		ready = true;
		return;
	}
	// two transactions of one attach finding it unready together would both shape - harmless, the
	// shaping is idempotent, but it is two batches of DDL; one of them waits instead
	static mutex readiness_lock;
	lock_guard<mutex> guard(readiness_lock);
	if (auto entry = cache.Get<MSSQLCatalogReadyEntry>(key)) {
		stats_length = entry->stats_length;
		ready = true;
		return;
	}
	// The conflict-check rewrite (specs/007 D1) and the inlined-deletion DDL (specs/006 D5b) are
	// recognised by exact match. A ducklake bump that edits either would silently stop matching and
	// bring back what they fix, in paths only concurrent writers or flushes reach - so the mismatch
	// is made loud, once per attach.
	if (!InlinedDeletionDdlIsDuckLakes(*this)) {
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
	auto markers = ReadCatalogMarkers();
	if (!markers.present) {
		// being created: DuckLake's DDL is still to come, and InitializeDuckLake (1.0) or the next
		// transaction's first query (1.1) shapes it. Not marked ready, so that one asks again.
		return;
	}
	// Phase 2 is off by default because it is incomplete, not because it is wrong (specs/005 D4, D5).
	if (ServerCommitEnabled()) {
		catalog.SetRetrialsServerSide(true);
	}
	// A catalog this build did not shape - an older build's, one a stock manager created at format
	// 1.1, or one whose shaping failed partway - is readable but not writable without the keys, and
	// the failure would surface much later as "requires a table with a primary key". One round trip
	// says whether there is anything to do.
	if (!markers.shape_current) {
		EnsureCatalogShape();
		// the shaping records the catalog's limits when it had none
		markers = ReadCatalogMarkers();
	}
	auto entry = make_shared_ptr<MSSQLCatalogReadyEntry>();
	if (!markers.limits.empty()) {
		entry->stats_length = LengthOf(LengthClass::STATS, CatalogLengths::Parse(markers.limits));
	}
	stats_length = entry->stats_length;
	cache.Put(key, entry);
	ready = true;
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
	inlined_table_names[table.id.index] = {commit_snapshot.snapshot_id, table_name};
	if (base_ddl.empty()) {
		return table_name;
	}

	string columns;
	for (auto &column : table.columns) {
		columns += StringUtil::Format(", %s %s", SQLIdentifier(column.name),
		                              TSQLColumnType(DuckLakeTypes::FromString(column.type)));
	}
	// An inlined row is updated (its end_snapshot is set) and deleted, so this table needs a key for
	// the same reason the catalog's own tables do. The metadata columns are named by the catalog's
	// DuckLake format (design/005).
	auto names = InlinedColumnNames(transaction);
	// Created outside the transaction (below), so a commit that fails after this leaves the table
	// behind, unregistered - and the next table to take the same id and schema version takes the
	// same name, finds it there, and would write into another table's columns. A table of this name
	// that no row of ducklake_inlined_data_tables names is such a leftover: asked on this
	// transaction's connection - its own locks (the shaping's ALTERs on a new catalog among them)
	// would block any other - and dropped by the statement that creates the table afresh.
	const auto schema_literal =
	    DuckLakeUtil::SQLLiteralToString(transaction.GetCatalog().MetadataSchemaName().GetIdentifierName());
	const auto name_literal = DuckLakeUtil::SQLLiteralToString(table_name);
	auto probe = transaction.GetConnection().Query(StringUtil::Format(
	    "SELECT leftover FROM mssql_scan_unsafe(%s, %s, columns := {'leftover': 'BIGINT'})", CatalogLiteral(),
	    DuckLakeUtil::SQLLiteralToString(StringUtil::Format(
	        "SELECT CAST(CASE WHEN OBJECT_ID(QUOTENAME(%s) + '.' + QUOTENAME(%s)) IS NOT NULL AND NOT EXISTS "
	        "(SELECT 1 FROM %s.ducklake_inlined_data_tables WHERE table_name = %s) THEN 1 ELSE 0 END AS BIGINT) AS "
	        "leftover",
	        schema_literal, name_literal, SchemaIdentifier(), name_literal))));
	if (probe->HasError()) {
		probe->GetErrorObject().Throw("Failed to create the inlined data table: ");
	}
	auto probed = probe->Fetch();
	const bool leftover = probed && probed->size() == 1 && probed->GetValue(0, 0).GetValue<int64_t>() == 1;
	auto statement = StringUtil::Format(
	    "%sIF OBJECT_ID(QUOTENAME(%s) + '.' + QUOTENAME(%s)) IS NULL "
	    "CREATE TABLE %s.%s(%s BIGINT NOT NULL, %s BIGINT NOT NULL, %s BIGINT%s, "
	    "CONSTRAINT %s PRIMARY KEY (%s, %s));",
	    leftover ? StringUtil::Format("DROP TABLE %s.%s; ", SchemaIdentifier(), SQLIdentifier(table_name)) : string(),
	    schema_literal, name_literal, SchemaIdentifier(), SQLIdentifier(table_name), SQLIdentifier(names.row_id),
	    SQLIdentifier(names.begin_snapshot), SQLIdentifier(names.end_snapshot), columns,
	    SQLIdentifier("pk_" + table_name), SQLIdentifier(names.row_id), SQLIdentifier(names.begin_snapshot));
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
	// Recorded as well, so DuckLake's own clear after the batch stays the targeted one rather than
	// falling back to re-reading the whole schema - and marked refreshed, so that clear does not do
	// this one again.
	tables_pending_cache_refresh.push_back(table_name);
	tables_already_refreshed.insert(table_name);
	return table_name;
}

} // namespace duckdb
