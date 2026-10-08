#include "mssql_metadata_manager.hpp"

#include <regex>
#include <mutex>
#include "duckdb/storage/object_cache.hpp"
#include "mssql_metadata_internal.hpp"

#include "common/ducklake_types.hpp"
#include "common/ducklake_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/database.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_stats.hpp"
#include "storage/ducklake_table_entry.hpp"
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
//! DuckLake writes the query in two forms - `include_exactness` adds min_is_exact/max_is_exact to
//! both arms of the union, and a 1.1 catalog asks for that one - so both are kept here, assembled
//! from the same pieces the base assembles them from.
constexpr const char *CONFLICT_HEAD = R"(
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
    NULL AS extra_stats)";
constexpr const char *CONFLICT_MID = R"(
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
    extra_stats)";
constexpr const char *CONFLICT_TAIL = R"(
FROM {METADATA_CATALOG}.ducklake_table_stats
LEFT JOIN {METADATA_CATALOG}.ducklake_table_column_stats
    USING (table_id)
ORDER BY table_id NULLS FIRST;
	)";

string DuckLakeConflictCheckQuery(bool exactness) {
	if (!exactness) {
		return string(CONFLICT_HEAD) + CONFLICT_MID + CONFLICT_TAIL;
	}
	return string(CONFLICT_HEAD) + ",\n    NULL AS min_is_exact,\n    NULL AS max_is_exact" + CONFLICT_MID +
	       ",\n    min_is_exact,\n    max_is_exact" + CONFLICT_TAIL;
}

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
//! A stats row whose sizes are NULL is returned as the base returns it: FillMissingTableSizes asks
//! for those after.
//!
//! It reads every table's stats inside a writing transaction, so with concurrent writers it can meet
//! one in a deadlock (error 1205) - and DuckLake runs it before a retry attempt counts as retryable,
//! so its losing would fail the commit. It runs at HIGH deadlock priority: the other side, a commit
//! batch (back at NORMAL, RunCommitBatch), loses instead, and its 1205 is retried
//! (IsRetryableCommitError).
//!
//! The inner text travels inside a DuckDB string literal, so each of its own quotes is doubled once.
string MSSQLConflictCheckQuery(bool exactness) {
	return StringUtil::Format(
	    R"(
FROM mssql_scan_unsafe({METADATA_CATALOG_NAME_LITERAL}, '
SET DEADLOCK_PRIORITY HIGH;
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
       CAST(NULL AS VARCHAR(MAX)) AS extra_stats%s
FROM (SELECT TOP 1 * FROM {METADATA_SCHEMA_ESCAPED}.ducklake_snapshot ORDER BY snapshot_id DESC) s
UNION ALL
SELECT NULL, NULL, NULL, NULL, NULL,
       ts.table_id, cs.column_id, ts.record_count, ts.next_row_id, ts.file_size_bytes,
       cs.contains_null, cs.contains_nan, cs.min_value, cs.max_value, cs.extra_stats%s
FROM {METADATA_SCHEMA_ESCAPED}.ducklake_table_stats ts
LEFT JOIN {METADATA_SCHEMA_ESCAPED}.ducklake_table_column_stats cs ON cs.table_id = ts.table_id
ORDER BY table_id', columns := {
    'snapshot_id': 'BIGINT', 'schema_version': 'BIGINT', 'next_catalog_id': 'BIGINT',
    'next_file_id': 'BIGINT', 'changes': 'VARCHAR', 'table_id': 'BIGINT', 'column_id': 'BIGINT',
    'record_count': 'BIGINT', 'next_row_id': 'BIGINT', 'file_size_bytes': 'BIGINT',
    'contains_null': 'BOOLEAN', 'contains_nan': 'BOOLEAN', 'min_value': 'VARCHAR',
    'max_value': 'VARCHAR', 'extra_stats': 'VARCHAR'%s}))",
	    exactness ? ",\n       CAST(NULL AS BIT) AS min_is_exact, CAST(NULL AS BIT) AS max_is_exact" : "",
	    exactness ? ", cs.min_is_exact, cs.max_is_exact" : "",
	    exactness ? ", 'min_is_exact': 'BOOLEAN', 'max_is_exact': 'BOOLEAN'" : "");
}

} // namespace

DuckLakeInlinedColNames InlinedColumnNames(DuckLakeTransaction &transaction) {
	return DuckLakeInlinedColNames(transaction.GetCatalog().SupportsV1_1Metadata());
}

bool ConflictCheckQueryIsDuckLakes() {
	return DuckLakeMetadataManager::GetSnapshotAndStatsAndChangesQuery(false) == DuckLakeConflictCheckQuery(false) &&
	       DuckLakeMetadataManager::GetSnapshotAndStatsAndChangesQuery(true) == DuckLakeConflictCheckQuery(true);
}

void MSSQLMetadataManager::CreateInlinedDeletionTable(const string &table_name) {
	// the inlined DELETION table keeps the bare names on 1.1 - only the inlined DATA tables took the
	// `_ducklake_` prefix (ducklake_metadata_manager.cpp:3719 writes this DDL bare); design/005
	auto statement = StringUtil::Format(
	    "IF OBJECT_ID(QUOTENAME(%s) + '.' + QUOTENAME(%s)) IS NULL "
	    "CREATE TABLE %s.%s(file_id BIGINT NOT NULL, row_id BIGINT NOT NULL, begin_snapshot BIGINT NOT NULL, "
	    "CONSTRAINT %s PRIMARY KEY (file_id, row_id, begin_snapshot));",
	    SQLString::ToString(transaction.GetCatalog().MetadataSchemaName().GetIdentifierName()),
	    SQLString::ToString(table_name), SchemaIdentifier(), SQLIdentifier(table_name),
	    SQLIdentifier("pk_" + table_name));
	RunServerSideOutsideTransaction(statement, "Failed to create the inlined deletion table: ");
	InvalidateTableCache(table_name);
	tables_pending_cache_refresh.push_back(table_name);
	tables_already_refreshed.insert(table_name);
}

//! The join and filter of BuildCatalogForSnapshot's tables+columns statement, verbatim, and the same
//! with the column filter inside the joined table.
static constexpr const char *DUCKLAKE_LOAD_COLUMNS_JOIN =
    "LEFT JOIN {METADATA_CATALOG}.ducklake_column col USING (table_id)\n"
    "WHERE {SNAPSHOT_ID} >= tbl.begin_snapshot AND ({SNAPSHOT_ID} < tbl.end_snapshot OR tbl.end_snapshot IS NULL)\n"
    "  AND (({SNAPSHOT_ID} >= col.begin_snapshot AND ({SNAPSHOT_ID} < col.end_snapshot OR col.end_snapshot IS NULL)) "
    "OR column_id IS NULL)\n";
static constexpr const char *MSSQL_LOAD_COLUMNS_JOIN =
    "LEFT JOIN (SELECT * FROM {METADATA_CATALOG}.ducklake_column WHERE {SNAPSHOT_ID} >= begin_snapshot AND "
    "({SNAPSHOT_ID} < end_snapshot OR end_snapshot IS NULL)) col USING (table_id)\n"
    "WHERE {SNAPSHOT_ID} >= tbl.begin_snapshot AND ({SNAPSHOT_ID} < tbl.end_snapshot OR tbl.end_snapshot IS NULL)\n";

unique_ptr<QueryResult> MSSQLMetadataManager::Query(DuckLakeSnapshot snapshot, string &query) {
	EnsureReady();
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
	if (ConflictRewriteEnabled()) {
		for (bool exactness : {false, true}) {
			if (query == DuckLakeConflictCheckQuery(exactness)) {
				query = MSSQLConflictCheckQuery(exactness);
				break;
			}
		}
	}
	// The catalog load's tables+columns statement (BuildCatalogForSnapshot) filters the columns AFTER
	// its LEFT JOIN - `((<visible>) OR column_id IS NULL)` - where no filter can be pushed below the
	// join, so every load read ALL of ducklake_column: 293 loads of a 300-table lake brought 3.6M
	// rows off the server, 3.95 s of its 4.4 s (specs/015). The same filter inside the joined table
	// is pushed into the scan, and the server returns the visible columns only. The two differ only
	// for a visible table without one visible column, which DuckLake never writes - and the base
	// would then reject it as "does not have any columns" either way. Matched on the template's exact
	// text: a ducklake bump that edits it leaves the statement as it is, merely slower.
	if (!LoadRewriteDisabled() && query.find(DUCKLAKE_LOAD_COLUMNS_JOIN) != string::npos) {
		query = StringUtil::Replace(query, DUCKLAKE_LOAD_COLUMNS_JOIN, MSSQL_LOAD_COLUMNS_JOIN);
	}
	// a write DuckLake sends through Query - the expiry's, the cleanup's, the flush's DELETEs -
	// takes the commit batch's T-SQL path when it is one of its families (specs/014 D3c)
	if (auto written = TryRewriteWrite(snapshot, query)) {
		return written;
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
static string ServerScan(const string &tsql, const string &columns) {
	return "FROM mssql_scan_unsafe({METADATA_CATALOG_NAME_LITERAL}, '" + StringUtil::Replace(tsql, "'", "''") +
	       "', columns := " + columns + ")";
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
	// a CASE over two integer literals is an `int` on the server, so INTEGER and not BIGINT
	auto query = ServerScan(
	    StringUtil::Format("SELECT CASE WHEN OBJECT_ID('{METADATA_SCHEMA_ESCAPED}.%s') IS NULL THEN 0 ELSE 1 END "
	                       "AS present",
	                       table_name),
	    "{'present': 'INTEGER'}");
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

unique_ptr<QueryResult> MSSQLMetadataManager::Query(string &query) {
	EnsureReady();
	// the snapshot-less path: what the expiry, the cleanup and the flush DELETE through (specs/014
	// D3c) - a recognised write goes as T-SQL, everything else to the base
	if (auto written = TryRewriteWrite(query)) {
		return written;
	}
	return DuckLakeMetadataManager::Query(query);
}

//===--------------------------------------------------------------------===//
// The file list on the server (specs/015)
//===--------------------------------------------------------------------===//

namespace {

//! The T-SQL type a stats string is compared as, and the bounds that stand in for one the server
//! cannot read. Empty means "no pruning on this column" - DuckLake's own convention for a cast a
//! backend cannot express, which the postgres manager uses the same way.
struct TSQLStatsType {
	string type;
	string lowest;
	string highest;
};

TSQLStatsType StatsTypeFor(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
		return {"BIGINT", "CAST(-9223372036854775808 AS BIGINT)", "CAST(9223372036854775807 AS BIGINT)"};
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
		// DECIMAL(38,0) holds every UBIGINT and HUGEINT up to 10^38; a stats string past that does not
		// cast, and its unknown bound then excludes nothing
		return {"DECIMAL(38, 0)", "CAST(-99999999999999999999999999999999999999 AS DECIMAL(38, 0))",
		        "CAST(99999999999999999999999999999999999999 AS DECIMAL(38, 0))"};
	case LogicalTypeId::DECIMAL: {
		auto width = DecimalType::GetWidth(type);
		auto scale = DecimalType::GetScale(type);
		auto nines = string(width - scale, '9') + (scale > 0 ? "." + string(scale, '9') : "");
		if (width == scale) {
			nines = "0." + string(scale, '9');
		}
		auto t = StringUtil::Format("DECIMAL(%d, %d)", width, scale);
		return {t, StringUtil::Format("CAST(-%s AS %s)", nines, t), StringUtil::Format("CAST(%s AS %s)", nines, t)};
	}
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return {"FLOAT", "CAST(-1.79E+308 AS FLOAT)", "CAST(1.79E+308 AS FLOAT)"};
	case LogicalTypeId::DATE:
		return {"DATE", "CAST('0001-01-01' AS DATE)", "CAST('9999-12-31' AS DATE)"};
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
		return {"DATETIME2(6)", "CAST('0001-01-01 00:00:00' AS DATETIME2(6))",
		        "CAST('9999-12-31 23:59:59.999999' AS DATETIME2(6))"};
	case LogicalTypeId::TIMESTAMP_TZ:
		return {"DATETIMEOFFSET(6)", "CAST('0001-01-01 00:00:00 +14:00' AS DATETIMEOFFSET(6))",
		        "CAST('9999-12-31 23:59:59.999999 -14:00' AS DATETIMEOFFSET(6))"};
	case LogicalTypeId::BOOLEAN:
		return {"BIT", "CAST(0 AS BIT)", "CAST(1 AS BIT)"};
	default:
		// TIMESTAMP_NS and TIME_NS lose digits in DATETIME2/TIME(7); TIME, TIME_TZ, INTERVAL and the
		// rest are not compared - no pruning on them, which is always correct
		return {};
	}
}

bool IsAscii(const string &text) {
	for (auto c : text) {
		if (static_cast<unsigned char>(c) >= 0x80) {
			return false;
		}
	}
	return true;
}

//! What FileListToTSQL made of a query: T-SQL; a known limit, which keeps the catalog path quietly;
//! or something it does not know, which is the strict guard's business.
enum class FileListOutcome { TSQL, KNOWN_LIMIT, UNKNOWN };

//! The closed list of rewrites that turn DuckLake's file-list query into T-SQL, and the check that
//! nothing else is left.
FileListOutcome FileListToTSQL(string &query) {
	// `col_N_stats.contains_nan` stands alone as a predicate in DuckDB SQL; a BIT is not one in T-SQL
	query = std::regex_replace(query, std::regex(R"((\bcol_\d+_stats\.contains_nan)\b(?!\s*(=|<>|IS\b)))"), "($1 = 1)");
	// the bucket-partition clause's `IS DISTINCT FROM n` (SQL Server has it only from 2022)
	query = std::regex_replace(query, std::regex(R"((\bdata\.partition_id) IS DISTINCT FROM (\d+))"),
	                           "($1 IS NULL OR $1 <> $2)");
	// Top-N's `ORDER BY e ASC|DESC NULLS LAST`, the last line DuckLake may add
	query = std::regex_replace(query, std::regex(R"(\nORDER BY (.+) (ASC|DESC) NULLS LAST$)"),
	                           "\nORDER BY CASE WHEN $1 IS NULL THEN 1 ELSE 0 END, $1 $2");
	// whatever DuckDB syntax a future ducklake adds, and text a VARCHAR literal could not carry, is not
	// ours to translate by guessing
	static const vector<string> foreign = {"::",     " NULLS ", " DISTINCT FROM ", "LIST(",   "list_", "struct_",
	                                       "regexp", "->",      "TRY_CAST(",       " ILIKE ", "{'"};
	for (auto &token : foreign) {
		if (query.find(token) != string::npos) {
			// our casts are written as TRY_CONVERT, so a TRY_CAST left over is DuckLake's and DuckDB-typed
			return FileListOutcome::UNKNOWN;
		}
	}
	// non-ASCII text outside our own N literals - a partition value DuckLake wrote as a plain literal -
	// would lose characters in a database of another code page
	auto outside_n_literals = std::regex_replace(query, std::regex(R"(N'(?:[^']|'')*')"), "N''");
	return IsAscii(outside_n_literals) ? FileListOutcome::TSQL : FileListOutcome::KNOWN_LIMIT;
}

} // namespace

string MSSQLMetadataManager::AddFileListParameter(const string &value, const string &declaration) {
	auto name = "p" + to_string(file_list_parameters.size() + 1);
	file_list_parameters.push_back({name, value, declaration});
	return "@" + name;
}

string MSSQLMetadataManager::CastValueToTarget(const Value &value, const LogicalType &type) {
	if (!building_tsql_file_list) {
		// DuckLake's own, for the catalog path - repeated rather than called, because the base keeps
		// it private. Re-audit at a ducklake bump (DuckLakeMetadataManager::CastValueToTarget).
		auto finite = (value.type().id() != LogicalTypeId::FLOAT && value.type().id() != LogicalTypeId::DOUBLE) ||
		              Value::IsFinite(value.GetValue<double>());
		if (type.IsNumeric() && finite) {
			return value.ToString();
		}
		return SQLString::ToString(value.ToString());
	}
	// On the server every constant is a parameter: one plan for every value, with or without the
	// database's forced parameterization (specs/012 is best-effort and has an opt-out), and a string
	// travels as nvarchar - any text, compared under the stats column's UTF-8 BIN2 collation, which
	// wins over the parameter's - so no literal has to survive the database's code page.
	if (value.IsNull()) {
		return string();
	}
	auto text = value.ToString();
	if (text.find('\0') != string::npos) {
		return string();
	}
	if (RequiresValueComparison(type)) {
		auto target = StatsTypeFor(type);
		if (target.type.empty()) {
			return string();
		}
		if (type.IsNumeric()) {
			if ((value.type().id() == LogicalTypeId::FLOAT || value.type().id() == LogicalTypeId::DOUBLE) &&
			    !Value::IsFinite(value.GetValue<double>())) {
				return string();
			}
		} else if (type.id() == LogicalTypeId::BOOLEAN) {
			text = value.GetValue<bool>() ? "1" : "0";
		} else {
			if (!HasFourDigitYear(text)) {
				return string();
			}
			if (type.id() == LogicalTypeId::TIMESTAMP_TZ) {
				text = WithMinuteOffset(text);
			}
		}
		// passed as text and converted by the declaration, so the value is the server's reading of
		// exactly the text DuckDB printed - the same reading the stats strings get
		return AddFileListParameter(SQLString::ToString(text), target.type);
	}
	if (type.id() == LogicalTypeId::VARCHAR || type.id() == LogicalTypeId::UUID) {
		return AddFileListParameter(SQLString::ToString(text), "NVARCHAR(MAX)");
	}
	return string();
}

string MSSQLMetadataManager::CastStatsToTarget(const string &stats, const LogicalType &type, StatsCastType cast_type) {
	if (!building_tsql_file_list) {
		// DuckLake's own, repeated for the same reason (DuckLakeMetadataManager::CastStatsToTarget)
		if (RequiresValueComparison(type)) {
			return "TRY_CAST(" + stats + " AS " + type.ToString() + ")";
		}
		return stats;
	}
	if (!RequiresValueComparison(type)) {
		return (type.id() == LogicalTypeId::VARCHAR || type.id() == LogicalTypeId::UUID) ? stats : string();
	}
	auto target = StatsTypeFor(type);
	if (target.type.empty()) {
		return string();
	}
	auto text = type.id() == LogicalTypeId::TIMESTAMP_TZ
	                ? StringUtil::Format("(CASE WHEN %s LIKE '%%[+-][0-9][0-9]' THEN %s + ':00' ELSE %s END)", stats,
	                                     stats, stats)
	                : stats;
	auto cast = StringUtil::Format("TRY_CONVERT(%s, %s)", target.type, text);
	switch (cast_type) {
	case StatsCastType::MIN:
		// a bound the server cannot read must not exclude a file: DuckDB reads every value it wrote,
		// SQL Server does not (an infinity, a HUGEINT past 10^38, a year past 9999)
		return StringUtil::Format("COALESCE(%s, %s)", cast, target.lowest);
	case StatsCastType::MAX:
		return StringUtil::Format("COALESCE(%s, %s)", cast, target.highest);
	default:
		return cast;
	}
}

string MSSQLMetadataManager::GenerateFileListQuery(DuckLakeTableEntry &table, const FilterPushdownInfo *filter_info,
                                                   const vector<DuckLakeFileListDynamicFilter> &dynamic_filters,
                                                   const vector<idx_t> &runtime_filter_stats_columns,
                                                   FileListType file_list_type, const string &metadata_table_prefix) {
	auto catalog_path = [&]() {
		return DuckLakeMetadataManager::GenerateFileListQuery(
		    table, filter_info, dynamic_filters, runtime_filter_stats_columns, file_list_type, metadata_table_prefix);
	};
	if (ServerFileListDisabled()) {
		return catalog_path();
	}
	// DuckLake's builder, with the schema as the server names it and our casts (its constants
	// parameters). Its stats CTE bodies (GenerateFileColumnStatsCTEBody) carry the column id and the
	// table id as literals; both become parameters below, so the statement's text is one per shape.
	file_list_parameters.clear();
	string query;
	building_tsql_file_list = true;
	try {
		query = DuckLakeMetadataManager::GenerateFileListQuery(table, filter_info, dynamic_filters,
		                                                       runtime_filter_stats_columns, file_list_type,
		                                                       "{METADATA_SCHEMA_ESCAPED}");
	} catch (...) {
		building_tsql_file_list = false;
		throw;
	}
	building_tsql_file_list = false;
	{
		const std::regex cte_filter("WHERE column_id = (\\d+) AND table_id = " + to_string(table.GetTableId().index) +
		                            "\\b");
		string rewritten;
		auto last = query.cbegin();
		for (std::sregex_iterator it(query.begin(), query.end(), cte_filter), end; it != end; ++it) {
			rewritten.append(last, (*it)[0].first);
			auto column = AddFileListParameter((*it)[1].str(), "BIGINT");
			rewritten += "WHERE column_id = " + column + " AND table_id = @table_id";
			last = (*it)[0].second;
		}
		rewritten.append(last, query.cend());
		query = std::move(rewritten);
	}
	auto outcome = FileListToTSQL(query);
	if (outcome == FileListOutcome::KNOWN_LIMIT) {
		return catalog_path();
	}
	if (outcome == FileListOutcome::UNKNOWN) {
		if (StrictBatchEnabled()) {
			throw InvalidInputException("mssql_ducklake: a file-list query the server-side read does not cover "
			                            "(specs/015): %s",
			                            query);
		}
		return catalog_path();
	}
	// The rest of what DuckLake wrote into the text: this table's id, and the snapshot. Both as
	// parameters, so the statement's text is the same for every table and every snapshot.
	auto table_id = to_string(table.GetTableId().index);
	query = std::regex_replace(query, std::regex("\\btable_id\\s*=\\s*" + table_id + "\\b"), "table_id = @table_id");
	query = StringUtil::Replace(query, "{SNAPSHOT_ID}", "@snapshot");

	string params = "'snapshot': {SNAPSHOT_ID}, 'table_id': " + table_id;
	string declarations = "@snapshot BIGINT, @table_id BIGINT";
	for (auto &parameter : file_list_parameters) {
		params += ", '" + parameter.name + "': " + parameter.value;
		declarations += ", @" + parameter.name + " " + parameter.declaration;
	}

	// The shape, given: it follows from the same inputs the select list does (GetFileSelectList,
	// GetDeleteFileSelectList, GenerateFileListQuery), so the bind sends nothing to the server. A
	// ducklake bump that changes the select list makes the stream disagree, and that is an error
	// naming the statement - never a misread column.
	auto encrypted = query.find("AS data_encryption_key") != string::npos;
	auto file_columns = [&](const string &prefix) {
		string out = StringUtil::Format("'%s_path': 'VARCHAR', '%s_path_is_relative': 'BOOLEAN', "
		                                "'%s_file_size_bytes': 'BIGINT', '%s_footer_size': 'BIGINT'",
		                                prefix, prefix, prefix, prefix);
		if (encrypted) {
			out += StringUtil::Format(", '%s_encryption_key': 'VARCHAR'", prefix);
		}
		return out;
	};
	string columns;
	if (file_list_type == FileListType::EXTENDED) {
		columns = "'data_file_id': 'BIGINT', 'delete_file_id': 'BIGINT', 'record_count': 'BIGINT', " +
		          file_columns("data") + ", 'row_id_start': 'BIGINT', 'mapping_id': 'BIGINT', " + file_columns("del") +
		          ", 'del_format': 'VARCHAR', 'begin_snapshot': 'BIGINT'";
	} else {
		columns = "'data_file_id': 'BIGINT', " + file_columns("data") +
		          ", 'row_id_start': 'BIGINT', 'begin_snapshot': 'BIGINT', 'partial_max': 'BIGINT', "
		          "'mapping_id': 'BIGINT', " +
		          file_columns("del") + ", 'del_format': 'VARCHAR'";
		for (auto &column : runtime_filter_stats_columns) {
			columns += StringUtil::Format(
			    ", 'col_%llu_stats_min_value': 'VARCHAR', 'col_%llu_stats_max_value': 'VARCHAR'", column, column);
		}
	}
	return StringUtil::Format("SELECT * FROM mssql_scan_params_unsafe({METADATA_CATALOG_NAME_LITERAL}, %s, {%s}, %s, "
	                          "columns := {%s})",
	                          SQLString(query), params, SQLString(declarations), columns);
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
	return R"(FROM mssql_scan_unsafe({METADATA_CATALOG_NAME_LITERAL}, 'SELECT TOP 1 snapshot_id, )"
	       R"(schema_version, next_catalog_id, next_file_id FROM {METADATA_SCHEMA_ESCAPED}.ducklake_snapshot )"
	       R"(ORDER BY snapshot_id DESC', columns := {'snapshot_id': 'BIGINT', 'schema_version': 'BIGINT', )"
	       R"('next_catalog_id': 'BIGINT', 'next_file_id': 'BIGINT'}))";
}

} // namespace duckdb

//===--------------------------------------------------------------------===//
// The global table stats, cached per catalog (specs/019)
//===--------------------------------------------------------------------===//

namespace duckdb {

namespace {

//! Every table's global stats as of `as_of`, the last snapshot whose changes they include. Kept in the
//! instance's object cache under the attached database's oid (never reused within an instance), and
//! shared by its transactions - under `lock`, since one brings it up to date for all.
class MSSQLGlobalStatsEntry : public ObjectCacheEntry {
public:
	static string ObjectType() {
		return "mssql_ducklake_global_stats";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx();
	}
	std::mutex lock;
	bool loaded = false;
	idx_t as_of = 0;
	map<TableIndex, DuckLakeGlobalStatsInfo> tables;
};

//! A commit since the cached state touched more tables than this: read everything instead.
constexpr idx_t STATS_CACHE_MAX_REREADS = 64;

} // namespace

vector<DuckLakeGlobalStatsInfo> MSSQLMetadataManager::GetGlobalTableStats(DuckLakeSnapshot snapshot) {
	// DuckLake asks for every table's stats once per snapshot (174a6f56), and a commit into one table
	// is a new snapshot - so each commit read all of them: 1000 tables x 41 columns, 15-30 ms on the
	// server, plus the S locks it takes on every other writer's rows inside a writing transaction
	// (deadlocks, error 1205, with eight concurrent writers). A transaction that has written may be
	// reading its own uncommitted state: it reads past the cache, as the base does.
	auto client_context = transaction.context.lock();
	if (StatsCacheDisabled() || wrote_in_transaction || !client_context) {
		return DuckLakeMetadataManager::GetGlobalTableStats(snapshot);
	}
	auto &cache = ObjectCache::GetObjectCache(*client_context);
	auto key = StringUtil::Format("mssql_ducklake:global_stats:%llu", transaction.GetCatalog().GetAttached().oid);
	auto entry = cache.Get<MSSQLGlobalStatsEntry>(key);
	if (!entry) {
		entry = make_shared_ptr<MSSQLGlobalStatsEntry>();
		cache.Put(key, entry);
		entry = cache.Get<MSSQLGlobalStatsEntry>(key);
	}
	std::lock_guard<std::mutex> guard(entry->lock);
	const bool v1_1 = transaction.GetCatalog().SupportsV1_1Metadata();

	// The snapshots committed since the cached state, by any node: their changes say which tables'
	// stats moved. A gap in the ids (expired changes), a change this build cannot read, or too many
	// tables, and the answer is to read everything - which is what the base does every time.
	bool full = !entry->loaded;
	set<TableIndex> reread;
	set<TableIndex> dropped;
	idx_t last = entry->as_of;
	if (!full) {
		string changes_query = StringUtil::Format(
		    "FROM mssql_scan_unsafe({METADATA_CATALOG_NAME_LITERAL}, 'SELECT snapshot_id, CAST(changes_made AS "
		    "VARCHAR(MAX)) AS changes_made FROM {METADATA_SCHEMA_ESCAPED}.ducklake_snapshot_changes WHERE snapshot_id "
		    "> %llu ORDER BY snapshot_id', columns := {'snapshot_id': 'BIGINT', 'changes_made': 'VARCHAR'})",
		    entry->as_of);
		auto changes = Query(snapshot, changes_query);
		if (changes->HasError()) {
			changes->GetErrorObject().Throw("Failed to read the DuckLake snapshot changes for the stats cache: ");
		}
		while (!full) {
			auto chunk = changes->Fetch();
			if (!chunk || chunk->size() == 0) {
				break;
			}
			for (idx_t row = 0; row < chunk->size() && !full; row++) {
				auto id = chunk->GetValue(0, row).GetValue<int64_t>();
				if (idx_t(id) != last + 1) {
					full = true;
					break;
				}
				last = idx_t(id);
				auto text = chunk->GetValue(1, row);
				if (text.IsNull()) {
					continue;
				}
				try {
					auto info = SnapshotChangeInformation::ParseChangesMade(text.ToString());
					for (auto *tables :
					     {&info.inserted_tables, &info.tables_deleted_from, &info.altered_tables,
					      &info.tables_compacted, &info.tables_merge_adjacent, &info.tables_rewrite_delete,
					      &info.tables_inserted_inlined, &info.tables_deleted_inlined, &info.tables_flushed_inlined}) {
						reread.insert(tables->begin(), tables->end());
					}
					dropped.insert(info.dropped_tables.begin(), info.dropped_tables.end());
				} catch (std::exception &) {
					full = true;
				}
			}
		}
		full = full || reread.size() > STATS_CACHE_MAX_REREADS;
	}

	if (full) {
		// the last snapshot first: a commit landing between this and the read below is read again next
		// time, which costs one table, where reading it after could miss one for good
		string latest_query =
		    "FROM mssql_scan_unsafe({METADATA_CATALOG_NAME_LITERAL}, 'SELECT ISNULL(MAX(snapshot_id), 0) "
		    "AS s FROM {METADATA_SCHEMA_ESCAPED}.ducklake_snapshot_changes', columns := {'s': 'BIGINT'})";
		auto latest = Query(snapshot, latest_query);
		if (latest->HasError()) {
			latest->GetErrorObject().Throw("Failed to read the latest DuckLake snapshot for the stats cache: ");
		}
		auto chunk = latest->Fetch();
		auto as_of = chunk && chunk->size() > 0 ? idx_t(chunk->GetValue(0, 0).GetValue<int64_t>()) : 0;
		auto all = DuckLakeMetadataManager::GetGlobalTableStats(snapshot);
		entry->tables.clear();
		for (auto &table : all) {
			entry->tables[table.table_id] = table;
		}
		entry->as_of = as_of;
		entry->loaded = true;
		return all;
	}

	for (auto &table_id : dropped) {
		entry->tables.erase(table_id);
	}
	for (auto &table_id : reread) {
		auto stats_query = GlobalTableStatsQuery(v1_1, table_id.index);
		auto result = Query(snapshot, stats_query);
		auto stats = ParseGlobalTableStats(*result);
		FillMissingTableSizes(stats, [&](string query) { return Query(snapshot, query); });
		entry->tables.erase(table_id);
		for (auto &table : stats) {
			entry->tables[table.table_id] = table;
		}
	}
	entry->as_of = last;
	vector<DuckLakeGlobalStatsInfo> out;
	out.reserve(entry->tables.size());
	for (auto &table : entry->tables) {
		out.push_back(table.second);
	}
	return out;
}

} // namespace duckdb
