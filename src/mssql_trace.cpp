//===----------------------------------------------------------------------===//
// mssql_ducklake_trace: every metadata round trip of a DuckLake catalog, timed, to a file
//===----------------------------------------------------------------------===//
// RECON (specs/015): the instrument behind the per-operation comparison with postgres. It hangs on
// DuckLake's query callback, which ExecuteRaw calls for every read of every manager - postgres and
// sqlite too - and this manager's TracedQuery calls for what it sends past ExecuteRaw. Postgres'
// commit writes (postgres_execute) do not pass the callback; they are counted on the server.

#include "mssql_trace.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_catalog.hpp"
#include <chrono>
#include <fstream>
#include <mutex>

namespace duckdb {

namespace {

std::mutex trace_lock;
std::ofstream trace_file;
//! the whole text of a round trip past 5 s, beside the trace - the trace keeps 600 characters
std::ofstream full_file;

int64_t NowMicros() {
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
	    .count();
}

string OneLine(const string &text, idx_t limit) {
	auto line = text.substr(0, limit);
	for (auto &c : line) {
		if (c == '\n' || c == '\r' || c == '\t') {
			c = ' ';
		}
	}
	return line;
}

void Write(const string &line) {
	std::lock_guard<std::mutex> guard(trace_lock);
	if (trace_file.is_open()) {
		trace_file << line << '\n';
		trace_file.flush();
	}
}

//! mssql_ducklake_trace('lake', 'path'): start; mssql_ducklake_trace('lake', ''): stop
void TraceFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto lake = args.GetValue(0, 0);
	auto path = args.GetValue(1, 0).ToString();
	auto db = DatabaseManager::Get(context).GetDatabase(context, Identifier(lake.ToString()));
	if (!db || db->GetCatalog().GetCatalogType() != "ducklake") {
		throw InvalidInputException("mssql_ducklake_trace: '%s' is not an attached DuckLake catalog", lake.ToString());
	}
	auto &catalog = db->GetCatalog().Cast<DuckLakeCatalog>();
	{
		std::lock_guard<std::mutex> guard(trace_lock);
		if (trace_file.is_open()) {
			trace_file.close();
			full_file.close();
		}
		if (!path.empty()) {
			trace_file.open(path, std::ios::app);
			full_file.open(path + ".full", std::ios::app);
		}
	}
	if (path.empty()) {
		catalog.SetQueryCallback(nullptr);
	} else {
		catalog.SetQueryCallback([](const string &query, std::chrono::steady_clock::duration elapsed) {
			auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
			auto start = NowMicros() - ns / 1000;
			Write(StringUtil::Format("Q\t%lld\t%lld\t%llu\t%s", (long long)start, (long long)ns,
			                         (unsigned long long)query.size(), OneLine(query, 600)));
			if (ns > 5000000000LL) {
				std::lock_guard<std::mutex> guard(trace_lock);
				if (full_file.is_open()) {
					full_file << "=== " << start << " " << ns << "\n" << query << "\n";
					full_file.flush();
				}
			}
		});
	}
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, path);
}

//! mssql_ducklake_trace_mark('phase:x'): a line in the trace, at this moment
void TraceMarkFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto label = args.GetValue(0, 0).ToString();
	Write(StringUtil::Format("M\t%lld\t%s", (long long)NowMicros(), OneLine(label, 200)));
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, label);
}

} // namespace

void RegisterTraceFunctions(ExtensionLoader &loader) {
	ScalarFunction trace("mssql_ducklake_trace", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                     TraceFun);
	trace.SetStability(FunctionStability::VOLATILE);
	loader.RegisterFunction(trace);
	ScalarFunction mark("mssql_ducklake_trace_mark", {LogicalType::VARCHAR}, LogicalType::VARCHAR, TraceMarkFun);
	mark.SetStability(FunctionStability::VOLATILE);
	loader.RegisterFunction(mark);
}

} // namespace duckdb
