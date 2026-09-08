#pragma once

#include "storage/ducklake_metadata_manager.hpp"

namespace duckdb {

// The SQL Server metadata manager (specs/002). Like the postgres manager it only generates SQL -
// the mssql extension resolves `mssql_exec`/`mssql_scan` at runtime, so nothing here links it.
// Phase 1 (T-SQL dialect gate over Execute, own InitializeDuckLake with keys + filtered indexes,
// the inlining type matrix) lands on this class; the two overrides below are the invariants that
// hold whatever the phase.
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
};

} // namespace duckdb
