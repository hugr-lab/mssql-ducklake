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

//===--------------------------------------------------------------------===//
// The conflict check as one statement (specs/007)
//===--------------------------------------------------------------------===//

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

//! The replacement: the same question asked as ONE statement the server answers by itself.
//!
//! Two properties follow from that, and only the first is why it was written. A single statement is
//! evaluated against a single consistent state, so no part of it can disagree with any other part -
//! which fixes the crash above and, unlike patching the one branch that happened to read a table
//! twice, keeps fixing it if DuckLake's query ever reads any table twice again. And it is one round
//! trip: the DuckDB-SQL form sends a separate SELECT per table, measured at 25 batches against 14
//! for this one on the same conflict check (specs/007 D1).
//!
//! T-SQL differences from DuckLake's version, none of them optional: `JOIN ... ON` because `USING`
//! is not T-SQL; `TOP 1` in a derived table because a branch of a `UNION ALL` may not carry its own
//! `ORDER BY`; `CAST(NULL AS ...)` so the union resolves the column types rather than guessing from
//! an untyped NULL; and no `NULLS FIRST`, which SQL Server does not have and does not need, since it
//! sorts NULLs first ascending - which is what puts the snapshot row where the parser expects it.
//!
//! The inner text travels inside a DuckDB string literal, so each of its own quotes is doubled once.
constexpr const char *MSSQL_CONFLICT_CHECK_QUERY = R"(
FROM mssql_scan({METADATA_CATALOG_NAME_LITERAL}, '
SELECT s.snapshot_id, s.schema_version, s.next_catalog_id, s.next_file_id,
       COALESCE((SELECT STRING_AGG(changes_made, '','')
                 FROM {METADATA_SCHEMA_ESCAPED}.ducklake_snapshot_changes c
                 WHERE c.snapshot_id > {SNAPSHOT_ID}), '''') AS changes,
       CAST(NULL AS BIGINT) AS table_id,
       CAST(NULL AS BIGINT) AS column_id,
       CAST(NULL AS BIGINT) AS record_count,
       CAST(NULL AS BIGINT) AS next_row_id,
       CAST(NULL AS BIGINT) AS file_size_bytes,
       CAST(NULL AS BIT) AS contains_null,
       CAST(NULL AS BIT) AS contains_nan,
       CAST(NULL AS VARCHAR(MAX)) AS min_value,
       CAST(NULL AS VARCHAR(MAX)) AS max_value,
       CAST(NULL AS VARCHAR(MAX)) AS extra_stats
FROM (SELECT TOP 1 * FROM {METADATA_SCHEMA_ESCAPED}.ducklake_snapshot ORDER BY snapshot_id DESC) s
UNION ALL
SELECT NULL, NULL, NULL, NULL, NULL,
       ts.table_id, cs.column_id, ts.record_count, ts.next_row_id, ts.file_size_bytes,
       cs.contains_null, cs.contains_nan, cs.min_value, cs.max_value, cs.extra_stats
FROM {METADATA_SCHEMA_ESCAPED}.ducklake_table_stats ts
LEFT JOIN {METADATA_SCHEMA_ESCAPED}.ducklake_table_column_stats cs ON cs.table_id = ts.table_id
WHERE ts.record_count IS NOT NULL AND ts.file_size_bytes IS NOT NULL
ORDER BY table_id'))";

} // namespace

//! The DDL DuckLake's commit loop writes into the batch for a new inlined deletion table, verbatim
//! from WriteNewInlinedFileDeletesSqlBatch - matched exactly, the way the conflict check is
//! (specs/007), and guarded at attach the same way. Between the two halves sits the table id.
constexpr const char *INLINED_DELETE_DDL_HEAD =
    "CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.ducklake_inlined_delete_";
constexpr const char *INLINED_DELETE_DDL_TAIL = "(file_id BIGINT, row_id BIGINT, begin_snapshot BIGINT);\n";

bool InlinedDeletionDdlIsDuckLakes() {
	DuckLakeInlinedFileDeletionInfo probe;
	probe.table_id = TableIndex(7);
	vector<DuckLakeInlinedFileDeletionInfo> one;
	one.push_back(std::move(probe));
	auto generated = DuckLakeMetadataManager::WriteNewInlinedFileDeletesSqlBatch(one);
	return StringUtil::StartsWith(generated, string(INLINED_DELETE_DDL_HEAD) + "7" + INLINED_DELETE_DDL_TAIL);
}

bool ConflictCheckQueryIsDuckLakes() {
	return DuckLakeMetadataManager::GetSnapshotAndStatsAndChangesQuery() == DUCKLAKE_CONFLICT_CHECK_QUERY;
}

unique_ptr<QueryResult> MSSQLMetadataManager::Execute(DuckLakeSnapshot snapshot, string &query) {
	// The commit loop puts `CREATE TABLE IF NOT EXISTS ducklake_inlined_delete_<t>(...)` into the
	// batch the first time a table's file-backed rows are deleted inline (the base's
	// WriteNewInlinedFileDeletesSqlBatch; on this pin the loop calls that static directly, so the
	// virtual around it is never asked). Run as DuckDB DDL inside the transaction that makes a
	// keyless table, and the flush that later DELETEs from it is refused: "UPDATE/DELETE requires a
	// table with a primary key". So the statement is taken out of the batch here and the table is
	// created the manager's way - keyed, in autocommit, and made known to the extension before the
	// INSERT that follows it in the same batch (specs/006 D5b). A DDL that is not exactly this text
	// is left to the base; the attach-time guard says when that starts happening.
	const string head = INLINED_DELETE_DDL_HEAD;
	const string tail = INLINED_DELETE_DDL_TAIL;
	idx_t pos = 0;
	while ((pos = query.find(head, pos)) != string::npos) {
		auto digits = pos + head.size();
		auto digits_end = digits;
		while (digits_end < query.size() && StringUtil::CharacterIsDigit(query[digits_end])) {
			digits_end++;
		}
		if (digits_end == digits || query.compare(digits_end, tail.size(), tail) != 0) {
			pos = digits;
			continue;
		}
		// The loop writes this DDL into EVERY batch that deletes inline from the table - the static
		// that builds it cannot know the table exists - so the catalog-level cache decides whether
		// there is anything to do. The manager's table is committed the moment it is created, which
		// is why it can be recorded as existing at once, unlike the base's transactional one.
		auto table_id = TableIndex(std::stoull(query.substr(digits, digits_end - digits)));
		auto &catalog = transaction.GetCatalog();
		if (catalog.CheckInlinedDeletionTableCache(table_id, snapshot) != InlinedDeletionCacheResult::EXISTS) {
			CreateInlinedDeletionTable(InlinedFileDeletionTableName(table_id));
			catalog.CacheInlinedDeletionTableResult(table_id, snapshot, true);
		}
		query.erase(pos, digits_end + tail.size() - pos);
	}
	return DuckLakeMetadataManager::Execute(snapshot, query);
}

void MSSQLMetadataManager::CreateInlinedDeletionTable(const string &table_name) {
	auto statement = StringUtil::Format(
	    "IF OBJECT_ID(QUOTENAME(%s) + '.' + QUOTENAME(%s)) IS NULL "
	    "CREATE TABLE %s.%s(file_id BIGINT NOT NULL, row_id BIGINT NOT NULL, begin_snapshot BIGINT NOT NULL, "
	    "CONSTRAINT %s PRIMARY KEY (file_id, row_id, begin_snapshot));",
	    DuckLakeUtil::SQLLiteralToString(transaction.GetCatalog().MetadataSchemaName()),
	    DuckLakeUtil::SQLLiteralToString(table_name), SchemaIdentifier(), SQLIdentifier(table_name),
	    SQLIdentifier("pk_" + table_name));
	RunServerSideOutsideTransaction(statement, "Failed to create the inlined deletion table: ");
	InvalidateTableCache(table_name);
	tables_pending_cache_refresh.push_back(table_name);
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
	if (ConflictRewriteEnabled() && query == DUCKLAKE_CONFLICT_CHECK_QUERY) {
		query = MSSQL_CONFLICT_CHECK_QUERY;
	}
	return DuckLakeMetadataManager::Query(snapshot, query);
}

//===--------------------------------------------------------------------===//
// The read layer in T-SQL (specs/008)
//===--------------------------------------------------------------------===//

//! One T-SQL statement as the sole source of a DuckDB query - the only shape mssql v0.2.5 runs on
//! the pinned connection inside a transaction (design 002 section 3.1). The text keeps the base's
//! placeholders: {METADATA_SCHEMA_ESCAPED} and {SNAPSHOT_ID} are substituted by the base's Query
//! after this, on the finished statement, which is why the schema is the identifier form and not
//! the literal one - a literal would arrive with quotes the doubling below has already passed.
static string ServerScan(const string &tsql) {
	return "FROM mssql_scan({METADATA_CATALOG_NAME_LITERAL}, '" + StringUtil::Replace(tsql, "'", "''") + "')";
}

//! The first row's first column as an idx_t, or `absent` when the scan returned no row.
static idx_t ScalarOf(QueryResult &result, const string &context, idx_t absent) {
	if (result.HasError()) {
		result.GetErrorObject().Throw(context);
	}
	auto chunk = result.Fetch();
	if (!chunk || chunk->size() == 0 || chunk->GetValue(0, 0).IsNull()) {
		return absent;
	}
	return chunk->GetValue(0, 0).GetValue<idx_t>();
}

// Why only this one lookup goes to the server, and not the other four a read makes (the inlined and
// data-file row counts, the global stats, the schema version's snapshot). All five are single-table
// scalars and all five were moved, and measured three ways against each other (specs/008): the
// probe alone gives the whole read-side win - a first read of a table on a thousand-table catalog
// goes from 0.29-0.46 s to 0.06-0.08 s, because a miss on this probe through the catalog path makes
// the mssql extension reload the schema's metadata, 964 ms on that catalog - while moving the other
// four cost 30-45% on every commit whose stats refresh runs them. Their catalog scans are what warm
// the extension's metadata for exactly the tables the commit batch then UPDATEs; through mssql_scan
// nothing is warmed and the batch pays the load itself. So they stay on the catalog path.

string MSSQLMetadataManager::GetInlinedDeletionTableName(TableIndex table_id, DuckLakeSnapshot snapshot,
                                                         bool create_if_not_exists) {
	auto table_name = InlinedFileDeletionTableName(table_id);
	if (create_if_not_exists) {
		// Reached only by a pin whose commit loop asks the virtual WriteNewInlinedFileDeletes; this
		// one calls the static batch builder and the Execute seam catches the DDL instead. Either
		// way the table is created the manager's way, keyed (specs/006 D5b).
		if (created_deletion_tables.find(table_id.index) == created_deletion_tables.end()) {
			auto &catalog = transaction.GetCatalog();
			if (catalog.CheckInlinedDeletionTableCache(table_id, snapshot) != InlinedDeletionCacheResult::EXISTS) {
				CreateInlinedDeletionTable(table_name);
			}
			created_deletion_tables.insert(table_id.index);
		}
		return table_name;
	}

	// The read path. The catalog-level cache is the base's and is consulted the same way; what
	// differs is the probe behind a miss. The base runs `SELECT NULL FROM <table> LIMIT 1` through
	// the catalog and reads its error state as "absent" - and on a miss the mssql extension reloads
	// the whole schema's metadata to find out. OBJECT_ID is one round trip and one row either way.
	auto &catalog = transaction.GetCatalog();
	auto cached = catalog.CheckInlinedDeletionTableCache(table_id, snapshot);
	if (cached == InlinedDeletionCacheResult::EXISTS) {
		return table_name;
	}
	if (cached == InlinedDeletionCacheResult::DOES_NOT_EXIST) {
		return string();
	}
	auto query = ServerScan(StringUtil::Format(
	    "SELECT CASE WHEN OBJECT_ID('{METADATA_SCHEMA_ESCAPED}.%s') IS NULL THEN 0 ELSE 1 END AS present", table_name));
	auto result = transaction.Query(snapshot, query);
	auto present = ScalarOf(*result, "Failed to look up the inlined deletion table in DuckLake: ", 0) != 0;
	if (!present) {
		catalog.CacheInlinedDeletionTableResult(table_id, snapshot, false);
		return string();
	}
	// Seen through this transaction's own connection, so a table this transaction created but has
	// not committed is visible too. The catalog-level "exists" is permanent, and that create can
	// still roll back - so it is recorded only for tables some earlier transaction committed. The
	// base gets the same protection from its private per-transaction cache, checked before the
	// probe; we cannot see that cache, so the test is our own record of what we created.
	if (created_deletion_tables.find(table_id.index) == created_deletion_tables.end()) {
		catalog.CacheInlinedDeletionTableResult(table_id, snapshot, true);
	}
	return table_name;
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
	return R"(FROM mssql_scan({METADATA_CATALOG_NAME_LITERAL}, 'SELECT TOP 1 snapshot_id, )"
	       R"(schema_version, next_catalog_id, next_file_id FROM {METADATA_SCHEMA_ESCAPED}.ducklake_snapshot )"
	       R"(ORDER BY snapshot_id DESC'))";
}

} // namespace duckdb
