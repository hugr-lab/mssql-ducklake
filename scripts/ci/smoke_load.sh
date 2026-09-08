#!/usr/bin/env bash
# The artifact must LOAD, not only compile: symbol visibility, a runtime dependency that resolved on
# the build machine only, an init that throws - all pass a build and fail the first user. The built
# file is copied out of the tree and loaded by the CLI from a different directory, as an operator
# would. Two scenarios, because the bridge has a deps gate (specs/001):
#   1. with mssql loaded first, the bridge loads and answers mssql_ducklake_version()
#      (ducklake is statically linked into the build's CLI, so that side is already up);
#   2. without mssql, LOAD must fail with OUR message - a missing-symbol error or a crash would
#      surface here instead of the gate.
#
#   scripts/ci/smoke_load.sh [<extension file>] [<duckdb binary>] [<mssql extension file>]
set -euo pipefail
ext="${1:-build/release/extension/mssql_ducklake/mssql_ducklake.duckdb_extension}"
duckdb="${2:-build/release/duckdb}"
mssql_ext="${3:-build/release/extension/mssql/mssql.duckdb_extension}"
[ -f "$ext" ] || { echo "smoke_load: no extension at $ext" >&2; exit 1; }
[ -x "$duckdb" ] || { echo "smoke_load: no duckdb CLI at $duckdb" >&2; exit 1; }
[ -f "$mssql_ext" ] || { echo "smoke_load: no mssql extension at $mssql_ext" >&2; exit 1; }
ext_abs="$(cd "$(dirname "$ext")" && pwd)/$(basename "$ext")"
duckdb_abs="$(cd "$(dirname "$duckdb")" && pwd)/$(basename "$duckdb")"
mssql_abs="$(cd "$(dirname "$mssql_ext")" && pwd)/$(basename "$mssql_ext")"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cp "$ext_abs" "$tmp/mssql_ducklake.duckdb_extension"
cp "$mssql_abs" "$tmp/mssql.duckdb_extension"
cd "$tmp"

out="$("$duckdb_abs" -unsigned -csv -noheader -c "
LOAD '$tmp/mssql.duckdb_extension';
LOAD '$tmp/mssql_ducklake.duckdb_extension';
SELECT 'version=' || coalesce(mssql_ducklake_version(), '<null>');
" 2>&1)" || { echo "smoke_load: the artifact did not load beside mssql:" >&2; echo "$out" >&2; exit 1; }
grep -q '^version=' <<<"$out" || { echo "smoke_load: no version answer:" >&2; echo "$out" >&2; exit 1; }

# autoload may not quietly satisfy the gate: disable it, so the check is about the message
neg="$("$duckdb_abs" -unsigned -csv -noheader -c "
SET autoload_known_extensions = false;
LOAD '$tmp/mssql_ducklake.duckdb_extension';
" 2>&1)" && { echo "smoke_load: LOAD succeeded without mssql - the deps gate is gone" >&2; exit 1; }
grep -q "mssql_ducklake is a bridge" <<<"$neg" || {
	echo "smoke_load: LOAD without mssql failed, but not with the gate's message:" >&2
	echo "$neg" >&2
	exit 1
}
echo "smoke_load: ok ($out; gate refuses without mssql; size $(wc -c <"$ext_abs" | tr -d ' ') bytes)"
