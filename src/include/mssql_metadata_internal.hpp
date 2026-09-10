#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/string_util.hpp"

#include <cstdlib>

namespace duckdb {

// What the manager's four source files share and the class header has no reason to carry: the
// environment switches, one literal helper, and the guard on DuckLake's conflict-check query
// (specs/010). Header-only on purpose - inline functions with a function-local static are one
// instance per program, so each switch is still read once.

//! A T-SQL string literal, or NULL - the value travels inside the batch mssql_exec runs.
inline string TSQLLiteral(const string &value) {
	return "N'" + StringUtil::Replace(value, "'", "''") + "'";
}

//! The switches phase 2 is behind while it is measured. Read once - getenv on every commit
//! would be a syscall in the hot path.
inline bool ServerCommitEnabled() {
	static const bool enabled = getenv("MSSQL_DUCKLAKE_SERVER_COMMIT") != nullptr;
	return enabled;
}

//! The commit size at which the server-side apply starts paying for its bulk loads. Measured at
//! about sixteen data files (specs/005 D7); a setting because the crossover moves with latency, and
//! on a link slower than a loopback socket it moves down.
inline idx_t ServerCommitMinFiles() {
	static const idx_t threshold = []() -> idx_t {
		auto *env = getenv("MSSQL_DUCKLAKE_SERVER_COMMIT_MIN_FILES");
		if (!env) {
			return 16;
		}
		try {
			auto value = std::stoll(env);
			return value < 0 ? 0 : static_cast<idx_t>(value);
		} catch (const std::exception &) {
			return 16;
		}
	}();
	return threshold;
}

inline bool SkipSnapshotFetchEnabled() {
	static const bool enabled = getenv("MSSQL_DUCKLAKE_SERVER_COMMIT_SKIP_FETCH") != nullptr;
	return enabled;
}

//! Off switch for the conflict-check rewrite (specs/007 D1). It exists so the regression test can be
//! shown to fail without the rewrite - a test that cannot fail proves nothing, and this one did pass
//! without it until its sensitivity was checked. Read once, like the switches above: this sits on
//! the path every metadata query takes.
inline bool ConflictRewriteEnabled() {
	static const bool disabled = getenv("MSSQL_DUCKLAKE_NO_CONFLICT_REWRITE") != nullptr;
	return !disabled;
}

//! Whether DuckLake's conflict-check query is still the text the specs/007 rewrite recognises. The
//! constant lives with the rewrite in mssql_metadata_queries.cpp; ProbeServerCapabilities asks this
//! at attach so that a ducklake bump editing the query fails loudly (specs/007).
bool ConflictCheckQueryIsDuckLakes();

//! Whether DuckLake's commit loop still writes the inlined deletion table's DDL into the batch in the
//! text the Execute seam recognises (specs/006 D5b). Asked at attach for the same reason.
bool InlinedDeletionDdlIsDuckLakes();

} // namespace duckdb
