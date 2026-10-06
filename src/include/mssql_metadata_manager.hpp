#pragma once

#include "duckdb/common/unordered_set.hpp"
#include "duckdb/main/connection.hpp"
#include "storage/ducklake_metadata_manager.hpp"

namespace duckdb {

// The SQL Server metadata manager (specs/002, phase 1 in specs/004). Like the postgres manager it
// only generates SQL - the mssql extension resolves `mssql_exec`/`mssql_scan` at runtime, so
// nothing here links it. Unlike the postgres manager it does not intercept the commit batch: with
// the catalog keyed (see InitializeDuckLake) duckdb executes DuckLake's own SQL against SQL Server,
// so this class rewrites no SQL at all.
//
// One class, four source files, cut along what each does (specs/010): mssql_metadata_manager.cpp -
// the type matrix, talking to the server, the inlined table, the attach-time probe;
// mssql_catalog_shape.cpp - the keys, indexes, collations and the database option that shape a
// catalog; mssql_server_commit.cpp - phase 2, the commit staged and applied on the server;
// mssql_metadata_queries.cpp - the queries written in T-SQL, the conflict check and the read layer.
class MSSQLMetadataManager : public DuckLakeMetadataManager {
public:
	explicit MSSQLMetadataManager(DuckLakeTransaction &transaction);

	static unique_ptr<DuckLakeMetadataManager> Create(DuckLakeTransaction &transaction) {
		return make_uniq<MSSQLMetadataManager>(transaction);
	}

	//! Write a commit's data files through DuckLake's appender rather than as one SQL batch
	//! (specs/006 D1). Postgres and sqlite answer false here and we read that as "remote catalogs
	//! cannot" - measured, it is not so. The appender is not a second transport: duckdb v1.5.5 turns
	//! it into `INSERT INTO <table> FROM <chunk>` (duckdb/src/main/appender.cpp), which mssql plans
	//! as its own batched insert, the same wire shape the SQL batch uses. What it avoids is the
	//! path resolution: the batch resolves every file's path through DuckLake's UNCACHED static
	//! helper, one `ducklake_table` and one `ducklake_schema` query PER FILE, where the appender
	//! goes through the manager's cached one. Round trips per commit stop growing with its size -
	//! 65 flat against 2065 for a thousand data files, and 1.7x faster at that size.
	bool SupportsAppender() const override;
	//! SQL Server sysname is 128 characters
	idx_t MaxIdentifierLength() const override {
		return 128;
	}

	//! DuckLake's DDL, then the shaping at once (specs/004 D3). Only at format 1.0: at 1.1 DuckLake
	//! swaps a registered manager for a stock one for the attach transaction (specs/015 R1), so a
	//! 1.1 catalog is created by stock DDL and shaped on first use instead (EnsureReady).
	void InitializeDuckLake(bool has_explicit_schema, DuckLakeEncryption encryption) override;

	//! Drop the mssql extension's catalog cache, which DuckLake asks for after creating an inlined
	//! table (specs/004 D2). Scoped to the tables actually created where we know them - see the
	//! definition for why that is worth the bookkeeping.
	void ClearCache() override;
	//! Refresh the extension's cached metadata for ONE table, at the moment it is created rather
	//! than at the end of the commit - see the call site for why the timing matters.
	void InvalidateTableCache(const string &table_name);

	//! Two jobs. On the create path it is the other place DuckLake creates a table behind the mssql
	//! extension's back, and ClearCache needs to know which one. On the read path it replaces the
	//! base's existence probe - a catalog query whose error state means "absent", and which on a
	//! miss makes the extension reload the whole schema's metadata - with one mssql_scan of
	//! OBJECT_ID (specs/008 D1).
	string GetInlinedDeletionTableName(TableIndex table_id, DuckLakeSnapshot snapshot,
	                                   bool create_if_not_exists = false) override;

	//! The inlined table's DDL is ours: its column types are T-SQL, which the duckdb-parsed commit
	//! batch could not carry, so it is executed separately (specs/004 D2).
	string GetInlinedTableQueries(DuckLakeSnapshot commit_snapshot, const DuckLakeTableInfo &table,
	                              string &inlined_tables, string &inlined_table_queries) override;

	//! The inlining type matrix (specs/004 D4). A type SQL Server cannot hold exactly is stored as
	//! text and cast back on read.
	bool TypeIsNativelySupported(const LogicalType &type) override;
	//! VARIANT has no inlined representation here - DuckLake would abort the commit rather than
	//! fall back to a data file, so the column is declared un-inlinable up front (as postgres does).
	bool SupportsInlining(const LogicalType &type) override;
	//! DuckDB type names, deliberately: DuckLake puts this string into the `CAST(... AS <type>)` it
	//! writes into the commit batch, and that batch is parsed by duckdb. The T-SQL names live in
	//! TSQLColumnType, which only our own DDL uses.
	string GetColumnTypeInternal(const LogicalType &type) override;

	//! Phase 2 (specs/005), being built. The commit's rows are staged on the server and applied by
	//! one call; until that call exists these hand back to the client-side loop, so the fast path is
	//! opt-in and every refusal is a fallback rather than a failure.
	void ProbeServerCapabilities() override;
	//! Everything this manager used to do at attach, done instead on the first metadata query of a
	//! transaction of ours, once per attach (specs/015 R1): the guards on DuckLake's texts, the
	//! server-commit flag, the two markers read in one statement and the shaping when the stamp is
	//! behind. At format 1.1 the attach transaction runs on a stock manager, so nothing this manager
	//! needs may depend on running there. A catalog not created yet is left alone, unmarked.
	void EnsureReady();
	//! Read the latest snapshot through `mssql_scan` rather than through the attached catalog - the
	//! postgres manager's trick. Worth about a tenth of a repeat read, this being one of roughly four
	//! catalog queries a read makes (specs/005 D13).
	string GetLatestSnapshotQuery() const override;

	//! The read path (specs/015): the file list of a scan - DuckLake's query with its stats CTEs and
	//! pruning filters - run on the server as ONE statement, the way the postgres manager runs it in
	//! postgres, instead of through the catalog path in pieces. DuckLake builds it with our CTE body
	//! and our casts; a closed list of rewrites makes it T-SQL; anything outside that list keeps the
	//! catalog path.
	string GenerateFileListQuery(DuckLakeTableEntry &table, const FilterPushdownInfo *filter_info,
	                             const vector<DuckLakeFileListDynamicFilter> &dynamic_filters,
	                             const vector<idx_t> &runtime_filter_stats_columns, FileListType file_list_type,
	                             const string &metadata_table_prefix,
	                             const FileColumnStatsCTEBodyGenerator &generate_cte_body) override;
	//! The pruning casts, in T-SQL while the file list is being built for the server and DuckLake's
	//! own otherwise (the catalog-path fallback is DuckDB SQL).
	string CastValueToTarget(const Value &value, const LogicalType &type) override;
	string CastStatsToTarget(const string &stats, const LogicalType &type, StatsCastType cast_type) override;
	//! Set while GenerateFileListQuery builds the server's statement.
	bool building_tsql_file_list = false;
	//! The parameters that statement collects as it is built: the name, the value as a DuckDB literal
	//! (for the params STRUCT), and its T-SQL declaration.
	struct FileListParameter {
		string name;
		string value;
		string declaration;
	};
	vector<FileListParameter> file_list_parameters;
	string AddFileListParameter(const string &value, const string &declaration);

	//! Intercepts exactly one of DuckLake's queries - the commit loop's conflict check - and swaps it
	//! for a form that reads ducklake_snapshot once instead of twice (specs/007 D1). Everything else
	//! goes to the base untouched. Both query texts are constants in the .cpp; nothing outside needs
	//! them.
	unique_ptr<QueryResult> Query(DuckLakeSnapshot snapshot, string &query) override;
	//! The snapshot-less overload DuckLake's expiry, cleanup and flush use for their DELETEs: the
	//! same T-SQL path for a recognised write (specs/014 D3c).
	unique_ptr<QueryResult> Query(string &query) override;
	//! The commit batch, with the one statement in it that is not DuckDB's to run: the DDL of a new
	//! inlined deletion table, which the manager creates keyed and outside the transaction instead
	//! (specs/006 D5b). Everything else in the batch goes to the base as it is.
	unique_ptr<QueryResult> Execute(DuckLakeSnapshot snapshot, string &query) override;

	bool CanSkipSnapshotFetch(const TransactionChangeInformation &changes) const override;
	//! The client merge's statistics for the commit being applied, read under the apply's lock.
	string ClientMergedStatsSql(DuckLakeTransaction &flush_transaction, DuckLakeSnapshot &locked);
	void FlushChangesServerSide(DuckLakeTransaction &transaction, DuckLakeSnapshot transaction_snapshot,
	                            const TransactionChangeInformation &transaction_changes,
	                            const DuckLakeRetryConfig &retry_config) override;

	//! The collation every VARCHAR column of the catalog carries. UTF-8, so the server stores the
	//! bytes DuckDB already has; BIN2, so comparisons order by code point exactly as DuckDB does -
	//! which is what makes the min/max statistics DuckLake pushes into the server prune correctly.
	static constexpr const char *VARCHAR_COLLATION = "Latin1_General_100_BIN2_UTF8";

private:
	//! Put the commit's rows into `#temp` tables on this transaction's connection: DuckLake stages
	//! them into local duckdb tables, and each non-empty one is bulk-loaded across (specs/005 D1,
	//! D2). Inside the transaction, so a rollback takes the staging with it.
	//!
	//! Only called for a commit already known to be data files alone - the caller decides that from
	//! the transaction's change sets, before any of this runs.
	void StageCommit(DuckLakeTransaction &transaction);

	//! The local half of the above: DuckLake's staging into duckdb temporary tables, and the number
	//! of data files it produced. Nothing crosses the wire, so the count is free and the choice
	//! between the two commit paths can be made before paying for either.
	idx_t StageCommitLocally(DuckLakeTransaction &transaction, const DuckLakeSnapshot &snapshot,
	                         const DuckLakeRetryConfig &retry_config);

	//! The inlined deletion table for a lake table, keyed, created in autocommit and made known to
	//! the mssql extension at once - shared by the Execute seam and the create path of
	//! GetInlinedDeletionTableName (specs/006 D5b).
	void CreateInlinedDeletionTable(const string &table_name);
	//! One run of the commit batch's T-SQL, on the transaction's connection; the result carries an
	//! error the way the base's Execute does, so the commit loop's retry and rollback see the same
	//! thing (specs/014 D3).
	unique_ptr<QueryResult> RunCommitBatch(const string &tsql);
	//! A write DuckLake sends through Query rather than Execute - the expiry's and cleanup's DELETEs,
	//! the flush's - recognised by the same families as the batch and run as T-SQL; nullptr when it
	//! is not one (specs/014 D3c).
	unique_ptr<QueryResult> TryRewriteWrite(DuckLakeSnapshot snapshot, const string &query);
	//! The same, for a statement that carries no snapshot placeholders (the Query(string &) path);
	//! one that does is left to the base.
	unique_ptr<QueryResult> TryRewriteWrite(const string &query);
	//! The shared tail of the two: the statement with its placeholders resolved, or not a write.
	unique_ptr<QueryResult> RewriteWriteStatement(string statement);
	//! The T-SQL column type for an inlined column, from the matrix.
	string TSQLColumnType(const LogicalType &type) const;
	//! Keys, indexes and collations, written so that running them twice is a no-op.
	void EnsureCatalogShape();
	//! The 1.0 -> 1.1-dev1 migration, ours in T-SQL rather than DuckLake's statements run through
	//! duckdb: `ADD COLUMN {IF_NOT_EXISTS}` has no T-SQL form, so the upstream batch fails the moment
	//! a column is already there and the catalog silently stays at 1.0 (design/005). Overriding the
	//! three virtuals is the seam - their SQL is never touched.
	void MigrateV10(bool allow_failures = false) override;
	void MigrateV10Dev() override;
	void MigrateInlinedColumnNames(bool probe_renamed) override;
	//! The one implementation behind all three: every statement guarded, so running it twice is a
	//! no-op rather than an error.
	void MigrateToV1_1Dev1();
	//! The last step of that shaping, and the only one outside the transaction and allowed to fail:
	//! `PARAMETERIZATION FORCED` on the catalog's database (specs/012). Skipped when the
	//! `mssql_ducklake_forced_parameterization` setting is false.
	void ApplyForcedParameterization();
	//! What the server says about this catalog, in one statement: does it exist yet, is its shape
	//! this build's, and has this build's migration run on it (specs/015 R3).
	struct CatalogMarkers {
		bool present = false;
		bool shape_current = false;
		bool migration_current = false;
	};
	CatalogMarkers ReadCatalogMarkers();
	//! The server must have the UTF-8 BIN2 collation the shaping converts the catalog's strings to.
	void RequireUtf8Collation();
	//! This manager has already found the attach ready - one check per transaction at most.
	bool ready = false;
	//! The shape this build of the extension wants. Bumped whenever EnsureCatalogShape changes what
	//! it produces - a column type, a key, an index, a database option - so that a catalog shaped by
	//! an older build is brought up to it on the next attach instead of being left as it was. Starts
	//! at 2 because 1 is implicitly every catalog shaped before this stamp existed: those carry no
	//! property at all, read as older, and are converted once. 3 added forced parameterization of
	//! the catalog's database (specs/012); 4 keys the inlined deletion tables an older build left
	//! keyless (specs/014); 5 rebuilds ducklake_schema_versions' key over table_id as well (issue
	//! #30 - without it a commit touching two tables is refused by the server). 6 keys and collates
	//! what format 1.1-dev1 adds, where it exists - every statement is guarded by the table's
	//! existence, so the same version shapes a 1.0 catalog and a 1.1 one.
	static constexpr int64_t SHAPE_VERSION = 6;
	//! Where that version is recorded: an extended property on the catalog's own ducklake_metadata
	//! table - per catalog, invisible to DuckLake's queries, and gone the moment the catalog's
	//! tables are, which is what makes a recreated catalog shape itself again.
	static constexpr const char *SHAPE_VERSION_PROPERTY = "mssql_ducklake_shape";
	//! The migration's own marker, beside the stamp and independent of it (specs/015 R3): the format
	//! and the revision of our migration to it. DuckLake re-runs the migration of a dev format on
	//! every writable attach; an equal marker makes that a no-op. The revision moves when upstream
	//! adds to the format under the same name, which brings already-migrated catalogs along.
	static constexpr const char *MIGRATION_MARKER_PROPERTY = "mssql_ducklake_migration";
	static constexpr const char *MIGRATION_MARKER = "1.1-dev1/1";
	//! Run T-SQL through `mssql_exec` on a connection of the caller's choosing.
	void RunOn(Connection &connection, const string &tsql, const string &context);
	//! On this transaction's connection.
	void RunServerSide(const string &tsql, const string &context);
	//! On a connection of its own, in autocommit - for DDL whose table duckdb has to discover
	//! before this transaction commits.
	void RunServerSideOutsideTransaction(const string &tsql, const string &context);
	//! The schema the catalog lives in, quoted for T-SQL.
	string SchemaIdentifier() const;
	//! The same schema as a SQL literal - what `OBJECT_ID`, `SCHEMA_ID` and `sys.schemas` take.
	string SchemaLiteral() const;
	//! The name of the attached mssql catalog, as a SQL literal - `mssql_exec`'s first argument.
	string CatalogLiteral() const;

	//! Tables created through our own DDL since the last cache clear, so the clear can name them
	//! instead of dropping the whole schema's metadata. Empty means "we do not know", and the clear
	//! falls back to the schema.
	vector<string> tables_pending_cache_refresh;
	//! Of those, the ones already refreshed right after we created them (both creation sites must,
	//! for a commit that creates a table and writes into it). The clear at the end of the commit names
	//! them only so it does not fall back to the whole schema; refreshing them again is a wasted round
	//! trip per DDL commit, so it skips them.
	unordered_set<string> tables_already_refreshed;
	//! Tables whose inlined-deletion table THIS transaction created. The catalog-level "exists"
	//! cache is permanent, and a create can still roll back - so the read-path probe never records
	//! "exists" for one of these. See GetInlinedDeletionTableName.
	unordered_set<idx_t> created_deletion_tables;
};

} // namespace duckdb
