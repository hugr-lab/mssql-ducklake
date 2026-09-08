#include "mssql_metadata_manager.hpp"

namespace duckdb {

MSSQLMetadataManager::MSSQLMetadataManager(DuckLakeTransaction &transaction) : DuckLakeMetadataManager(transaction) {
}

} // namespace duckdb
