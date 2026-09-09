#pragma once

#include "storage/ducklake_metadata_manager.hpp"

namespace duckdb {

// The SQL Server metadata manager (specs/002, phase 1 in specs/004). Like the postgres manager it
// only generates SQL - the mssql extension resolves `mssql_exec`/`mssql_scan` at runtime, so
// nothing here links it.
class MSSQLMetadataManager : public DuckLakeMetadataManager {
public:
	explicit MSSQLMetadataManager(DuckLakeTransaction &transaction);

	static unique_ptr<DuckLakeMetadataManager> Create(DuckLakeTransaction &transaction) {
		return make_uniq<MSSQLMetadataManager>(transaction);
	}

	//! file inserts join the single commit batch instead (like postgres)
	bool SupportsAppender() const override {
		return false;
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

	//! Drop the mssql extension's catalog cache, which DuckLake asks for after a commit that created
	//! an inlined table - the one moment it is safe to (specs/004 D2).
	void ClearCache() override;

	//! The inlined table's DDL is ours (its types and collation are), and T-SQL cannot travel in a
	//! batch duckdb parses - so it is executed here, on the transaction's connection, and left out
	//! of the batch (specs/004 D2).
	string GetInlinedTableQueries(DuckLakeSnapshot commit_snapshot, const DuckLakeTableInfo &table,
	                              string &inlined_tables, string &inlined_table_queries) override;

	//! The inlining type matrix (specs/004 D4). A type SQL Server cannot hold exactly is stored as
	//! text and cast back on read; everything else gets a real column type.
	bool TypeIsNativelySupported(const LogicalType &type) override;
	string GetColumnTypeInternal(const LogicalType &type) override;

	//! The collation every VARCHAR column of the catalog carries. UTF-8, so the server stores the
	//! bytes DuckDB already has; BIN2, so comparisons order by code point exactly as DuckDB does -
	//! which is what makes the min/max statistics DuckLake pushes into the server prune correctly.
	static constexpr const char *VARCHAR_COLLATION = "Latin1_General_100_BIN2_UTF8";

	//! The result of rewriting one commit batch: the T-SQL to send, and whether it contains DDL -
	//! the mssql extension caches catalog metadata, and a table this batch creates is invisible to
	//! the reads that follow until that cache is dropped.
	struct TranspiledBatch {
		string sql;
		bool changes_schema = false;
	};

	//! Rewrite the DuckDB SQL that ducklake's non-virtual generators produced into T-SQL. Public
	//! for the unit tests; see specs/004 D2 for why a rewrite is unavoidable and why it is a
	//! scanner rather than a regex.
	static TranspiledBatch TranspileBatch(const string &query);

private:
	//! Run T-SQL on the metadata server through `mssql_exec`, on this transaction's connection.
	void RunServerSide(const string &tsql, const string &context);
	//! The same, on a connection of its own in autocommit - for DDL whose table duckdb has to
	//! discover before this transaction commits.
	void RunServerSideOutsideTransaction(const string &tsql, const string &context);
	//! The schema the catalog lives in, quoted for T-SQL.
	string SchemaIdentifier() const;
	//! The name of the attached mssql catalog, as a SQL literal - `mssql_exec`'s first argument.
	string CatalogLiteral() const;
};

} // namespace duckdb
