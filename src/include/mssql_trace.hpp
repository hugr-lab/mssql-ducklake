//===----------------------------------------------------------------------===//
// mssql_ducklake_trace (RECON, specs/015)
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {

class ExtensionLoader;

//! mssql_ducklake_trace('lake', 'path') and mssql_ducklake_trace_mark('label')
void RegisterTraceFunctions(ExtensionLoader &loader);

} // namespace duckdb
