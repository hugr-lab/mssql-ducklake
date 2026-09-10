#define DUCKDB_EXTENSION_MAIN

#include "mssql_ducklake_extension.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "mssql_metadata_manager.hpp"
#include "storage/ducklake_metadata_manager.hpp"

#include <mutex>

// DuckLake on SQL Server, batteries included (specs/002): this extension COMPILES DUCKLAKE IN -
// the whole untouched pinned source - and adds the mssql metadata manager beside the built-in
// postgres/sqlite ones. One image means the manager registry is ours by construction; the price is
// mutual exclusion with a stock ducklake, whose registry lives behind hidden symbols in its own
// image and whose surface (functions, the `ducklake` ATTACH prefix) would collide with the copy in
// here. Load order matters: load this extension BEFORE the first `ATTACH 'ducklake:...'`, or
// duckdb's autoloading resolves the prefix to stock ducklake first.

// ducklake's own entry point, compiled into this image (ducklake/src/ducklake_extension.cpp)
extern "C" void ducklake_duckdb_cpp_init(duckdb::ExtensionLoader &loader);

namespace duckdb {
namespace {

void MssqlDucklakeVersionFun(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(Value(MssqlDucklakeExtension().Version()));
}

void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();

	// the embedded copy cannot coexist with a loaded stock ducklake: same function names, same
	// ATTACH prefix, and each image reads its own manager registry
	if (db.ExtensionIsLoaded("ducklake")) {
		throw InvalidInputException(
		    "mssql_ducklake embeds ducklake and cannot be loaded together with the ducklake extension. "
		    "Use one of the two: LOAD mssql_ducklake (DuckLake incl. SQL Server catalogs) or LOAD ducklake "
		    "(stock, no SQL Server catalog support).");
	}

	// the manager's generated SQL runs through mssql_exec/mssql_scan, so the other half of the pair
	// must be around before the first ducklake:mssql attach - fail here, with the fix, not there
	if (!db.ExtensionIsLoaded("mssql") && !ExtensionHelper::TryAutoLoadExtension(db, "mssql")) {
		throw MissingExtensionException("mssql_ducklake needs the mssql extension (the metadata manager's SQL runs "
		                                "through mssql_exec/mssql_scan). Run: INSTALL mssql FROM community; LOAD "
		                                "mssql; and then LOAD mssql_ducklake;");
	}

	// the full ducklake surface: the `ducklake` ATTACH prefix, ducklake_* functions, secret type,
	// settings - registered by ducklake's own init, same image
	ducklake_duckdb_cpp_init(loader);

	// the registry is process-global while Load runs per database instance; a second Register of
	// the same key throws by design
	static std::once_flag register_once;
	std::call_once(register_once, [] { DuckLakeMetadataManager::Register("mssql", MSSQLMetadataManager::Create); });

	loader.SetDescription("DuckLake with SQL Server metadata catalog support (embeds ducklake)");
	// The one database-wide thing the manager does to a catalog's database, and so the one with an
	// opt-out (specs/012). Read when the catalog is shaped - at its creation, or at the first attach
	// with a build whose shape version is newer - not on every attach.
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption(
	    "mssql_ducklake_forced_parameterization",
	    "Set PARAMETERIZATION FORCED on a DuckLake catalog's SQL Server database when the catalog "
	    "is shaped; one plan per query shape instead of one per literal",
	    LogicalType::BOOLEAN, Value::BOOLEAN(true), nullptr, SetScope::GLOBAL);
	loader.RegisterFunction(
	    ScalarFunction("mssql_ducklake_version", {}, LogicalType::VARCHAR, MssqlDucklakeVersionFun));
}

} // namespace

void MssqlDucklakeExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string MssqlDucklakeExtension::Name() {
	return "mssql_ducklake";
}

std::string MssqlDucklakeExtension::Version() const {
#ifdef EXT_VERSION_MSSQL_DUCKLAKE
	return EXT_VERSION_MSSQL_DUCKLAKE;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(mssql_ducklake, loader) {
	duckdb::LoadInternal(loader);
}
}
