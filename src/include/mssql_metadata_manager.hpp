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

	//! The commit batch goes to the server as one raw T-SQL statement through `mssql_exec`, so
	//! duckdb's DML path - and its primary-key requirement - is never involved (specs/004 D1).
	unique_ptr<QueryResult> Execute(DuckLakeSnapshot snapshot, string &query) override;

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
	//! Placeholder substitution for the passthrough. Mirrors the base, except that
	//! `{METADATA_CATALOG}` becomes the schema identifier alone: the SQL runs inside SQL Server,
	//! where the attached catalog's name is not a prefix (the postgres manager does the same).
	void SubstitutePassthroughPlaceholders(DuckLakeSnapshot snapshot, string &query) const;
};

} // namespace duckdb
