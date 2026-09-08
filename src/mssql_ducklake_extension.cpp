#define DUCKDB_EXTENSION_MAIN

#include "mssql_ducklake_extension.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_helper.hpp"

// The bridge extension (specs/001): it contributes a DuckLake metadata manager for SQL Server, so a
// stock `ducklake` and a stock `mssql` can pair up without either repository changing. Loading the
// bridge IS the registration - there is nothing to call afterwards, and there is nothing here for a
// user who has not loaded both sides, so Load fails fast with the reason instead of deferring a
// worse error to the first ATTACH 'ducklake:mssql:...'.

namespace duckdb {
namespace {

void RequireDependency(DatabaseInstance &db, const string &name, const string &why) {
	if (db.ExtensionIsLoaded(name)) {
		return;
	}
	if (ExtensionHelper::TryAutoLoadExtension(db, name)) {
		return;
	}
	throw MissingExtensionException("mssql_ducklake is a bridge between the ducklake and mssql extensions and needs "
	                                "both loaded; '%s' is not (%s). Run: INSTALL %s; LOAD %s; LOAD mssql_ducklake;",
	                                name, why, name, name);
}

void MssqlDucklakeVersionFun(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(Value(MssqlDucklakeExtension().Version()));
}

void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	RequireDependency(db, "ducklake", "it owns the metadata-manager registry the bridge registers into");
	RequireDependency(db, "mssql", "the manager's generated SQL runs through mssql_exec/mssql_scan");

	// TODO(specs/001): version-gate against the loaded ducklake, then register MSSQLMetadataManager
	// into ducklake's registry - a direct DuckLakeMetadataManager::Register call in a static build,
	// dlsym into the loaded ducklake image in a loadable one.

	loader.SetDescription("DuckLake metadata catalog on SQL Server (bridge between ducklake and mssql)");
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
