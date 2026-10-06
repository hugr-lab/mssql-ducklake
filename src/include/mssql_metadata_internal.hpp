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

//! DuckDB prints a TIMESTAMPTZ with a bare hour offset (`+00`); SQL Server reads `+00:00`.
inline string WithMinuteOffset(const string &text) {
	auto n = text.size();
	if (n > 3 && (text[n - 3] == '+' || text[n - 3] == '-') && StringUtil::CharacterIsDigit(text[n - 2]) &&
	    StringUtil::CharacterIsDigit(text[n - 1])) {
		return text + ":00";
	}
	return text;
}

inline bool HasFourDigitYear(const string &text) {
	return text.size() >= 10 && StringUtil::CharacterIsDigit(text[0]) && StringUtil::CharacterIsDigit(text[1]) &&
	       StringUtil::CharacterIsDigit(text[2]) && StringUtil::CharacterIsDigit(text[3]) && text[4] == '-' &&
	       text[7] == '-';
}

//! Off switch for the catalog load's column filter moved into its join (specs/015), for measuring.
inline bool LoadRewriteDisabled() {
	static const bool disabled = getenv("MSSQL_DUCKLAKE_NO_LOAD_REWRITE") != nullptr;
	return disabled;
}

//! Off switch for the server-side file list (specs/015), so its absence can be measured.
inline bool ServerFileListDisabled() {
	static const bool disabled = getenv("MSSQL_DUCKLAKE_NO_SERVER_FILE_LIST") != nullptr;
	return disabled;
}

//! Off switch for the user's inlined rows in the T-SQL run (specs/015): they take DuckDB's DML path
//! again, one round trip of their own. For measuring, and for running the two against each other.
inline bool InlinedRowsInRunDisabled() {
	static const bool disabled = getenv("MSSQL_DUCKLAKE_NO_INLINED_TSQL") != nullptr;
	return disabled;
}

//! The size past which a T-SQL run goes as several calls, cut at statement boundaries, all on the
//! transaction's connection (specs/015). SQL Server parses and compiles a batch whole: one flush of
//! 1000 tables' inlined data was a 2.7 MB batch of ~3000 statements, superlinear in time (0.27 MB
//! 218 ms, 0.80 MB 1245 ms) and past the 2 GB server's memory (error 701). MSSQL_DUCKLAKE_RUN_LIMIT_KB
//! overrides it, for measuring.
inline idx_t RunLimitBytes() {
	static const idx_t limit = [] {
		auto *env = getenv("MSSQL_DUCKLAKE_RUN_LIMIT_KB");
		return static_cast<idx_t>(env ? std::strtoull(env, nullptr, 10) : 256) * 1024;
	}();
	return limit;
}

//! specs/014: an unrecognised catalog statement in the commit batch is an error rather than a
//! fallback to the base. On in the integration suite, so that a ducklake bump that adds a shape
//! fails the suite naming the statement; off for a user, whose catalog keeps working, slower.
inline bool StrictBatchEnabled() {
	static const bool enabled = getenv("MSSQL_DUCKLAKE_STRICT_BATCH") != nullptr;
	return enabled;
}

//! Off switch for the whole rewrite (specs/014): every statement goes to the base, the way it did
//! before. Exists so that the two paths can be run against each other - a test that cannot fail
//! proves nothing - and so that a user can take the rewrite out of a diagnosis.
inline bool BatchRewriteEnabled() {
	static const bool disabled = getenv("MSSQL_DUCKLAKE_NO_BATCH_REWRITE") != nullptr;
	return !disabled;
}

//! specs/014 D5: with the appender off, a commit's data-file rows, statistics and partition values
//! are INSERT statements in the batch, which the T-SQL run sends with everything else. A switch
//! while the two are measured against each other.
inline bool AppenderDisabled() {
	static const bool disabled = getenv("MSSQL_DUCKLAKE_NO_APPENDER") != nullptr;
	return disabled;
}

//! Whether DuckLake's conflict-check query is still the text the specs/007 rewrite recognises. The
//! constant lives with the rewrite in mssql_metadata_queries.cpp; ProbeServerCapabilities asks this
//! at attach so that a ducklake bump editing the query fails loudly (specs/007).
bool ConflictCheckQueryIsDuckLakes();

//! Whether DuckLake's commit loop still writes the inlined deletion table's DDL into the batch in the
//! text the Execute seam recognises (specs/006 D5b). Asked at attach for the same reason.
bool InlinedDeletionDdlIsDuckLakes(DuckLakeMetadataManager &manager);

//! The inlined tables' own metadata columns are named by the catalog's DuckLake format: bare on 1.0,
//! `_ducklake_`-prefixed from 1.1-dev1 (DuckLakeInlinedColNames). Our inlined DDL is ours to write,
//! so it has to follow the format rather than hard-code the 1.0 names (design/005).
DuckLakeInlinedColNames InlinedColumnNames(DuckLakeTransaction &transaction);

} // namespace duckdb
