#include "mssql_metadata_manager.hpp"

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
#include "mssql_metadata_internal.hpp"

namespace duckdb {

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

bool MSSQLMetadataManager::CanSkipSnapshotFetch(const TransactionChangeInformation &changes) const {
	// Measured (specs/005 D7), the apply saves little on round trips by itself - 15.2 against the
	// client loop's 16.0 per commit - because the loop's batches are mostly reads the apply makes too.
	// Skipping the fetch was meant to be the rest of the saving; measured, it is not
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

} // namespace duckdb
