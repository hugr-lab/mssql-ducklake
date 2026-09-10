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
	bool SupportsAppender() const override {
		return true;
	}
	//! SQL Server sysname is 128 characters
	idx_t MaxIdentifierLength() const override {
		return 128;
	}

	//! Our own initialization: DuckLake's DDL, then the T-SQL it cannot express - primary keys, the
	//! filtered indexes every versioned read wants, and a binary UTF-8 collation on the statistics
	//! columns the server compares (specs/004 D3). The keys are what let every UPDATE and DELETE in
	//! a commit run through duckdb, which is why this manager needs no SQL rewriting at all.
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
	//! Read the latest snapshot through `mssql_scan` rather than through the attached catalog - the
	//! postgres manager's trick. Worth about a tenth of a repeat read, this being one of roughly four
	//! catalog queries a read makes (specs/005 D13).
	string GetLatestSnapshotQuery() const override;

	//! Intercepts exactly one of DuckLake's queries - the commit loop's conflict check - and swaps it
	//! for a form that reads ducklake_snapshot once instead of twice (specs/007 D1). Everything else
	//! goes to the base untouched. Both query texts are constants in the .cpp; nothing outside needs
	//! them.
	unique_ptr<QueryResult> Query(DuckLakeSnapshot snapshot, string &query) override;

	bool CanSkipSnapshotFetch(const TransactionChangeInformation &changes) const override;
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

	//! The T-SQL column type for an inlined column, from the matrix.
	string TSQLColumnType(const LogicalType &type) const;
	//! Keys, indexes and collations, written so that running them twice is a no-op.
	void EnsureCatalogShape();
	//! The last step of that shaping, and the only one outside the transaction and allowed to fail:
	//! `PARAMETERIZATION FORCED` on the catalog's database (specs/012). Skipped when the
	//! `mssql_ducklake_forced_parameterization` setting is false.
	void ApplyForcedParameterization();
	//! Is that shaping already applied? Asked on every attach, so it is one query rather than the
	//! whole idempotent batch.
	bool CatalogShapeIsCurrent();
	//! The shape this build of the extension wants. Bumped whenever EnsureCatalogShape changes what
	//! it produces - a column type, a key, an index, a database option - so that a catalog shaped by
	//! an older build is brought up to it on the next attach instead of being left as it was. Starts
	//! at 2 because 1 is implicitly every catalog shaped before this stamp existed: those carry no
	//! property at all, read as older, and are converted once. 3 added forced parameterization of
	//! the catalog's database (specs/012).
	static constexpr int64_t SHAPE_VERSION = 3;
	//! Where that version is recorded: an extended property on the catalog's own ducklake_metadata
	//! table - per catalog, invisible to DuckLake's queries, and gone the moment the catalog's
	//! tables are, which is what makes a recreated catalog shape itself again.
	static constexpr const char *SHAPE_VERSION_PROPERTY = "mssql_ducklake_shape";
	//! Run T-SQL through `mssql_exec` on a connection of the caller's choosing.
	void RunOn(Connection &connection, const string &tsql, const string &context);
	//! On this transaction's connection.
	void RunServerSide(const string &tsql, const string &context);
	//! On a connection of its own, in autocommit - for DDL whose table duckdb has to discover
	//! before this transaction commits.
	void RunServerSideOutsideTransaction(const string &tsql, const string &context);
	//! The schema the catalog lives in, quoted for T-SQL.
	string SchemaIdentifier() const;
	//! The name of the attached mssql catalog, as a SQL literal - `mssql_exec`'s first argument.
	string CatalogLiteral() const;

	//! Tables created through our own DDL since the last cache clear, so the clear can name them
	//! instead of dropping the whole schema's metadata. Empty means "we do not know", and the clear
	//! falls back to the schema.
	vector<string> tables_pending_cache_refresh;
	//! Tables whose inlined-deletion table THIS transaction created. The catalog-level "exists"
	//! cache is permanent, and a create can still roll back - so the read-path probe never records
	//! "exists" for one of these. See GetInlinedDeletionTableName.
	unordered_set<idx_t> created_deletion_tables;
};

} // namespace duckdb
