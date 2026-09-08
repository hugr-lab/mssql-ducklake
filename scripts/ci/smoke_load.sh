#!/usr/bin/env bash
# The artifact must LOAD, not only compile: symbol visibility, a runtime dependency that resolved on
# the build machine only, an init that throws - all pass a build and fail the first user. The built
# file is copied out of the tree and loaded by the CLI from a different directory, as an operator
# would. Scenarios (specs/002):
#   1. beside mssql the extension loads, answers, and runs a full local-file lake cycle through the
#      embedded ducklake;
#   2. without mssql (autoload off), LOAD fails with OUR gate message;
#   3. with STOCK ducklake loaded first (INSTALL from the official repository - needs network;
#      skipped with a note when offline), LOAD fails with OUR exclusion message.
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

# 1. the happy path: load beside mssql, then a whole lake round trip on a local metadata file
out="$("$duckdb_abs" -unsigned -csv -noheader -c "
LOAD '$tmp/mssql.duckdb_extension';
LOAD '$tmp/mssql_ducklake.duckdb_extension';
ATTACH 'ducklake:$tmp/meta.ducklake' AS lake (DATA_PATH '$tmp/lake_files');
CREATE TABLE lake.t(i INTEGER);
INSERT INTO lake.t VALUES (1), (2);
SELECT 'sum=' || sum(i) FROM lake.t;
SELECT 'version=' || coalesce(mssql_ducklake_version(), '<null>');
" 2>&1)" || { echo "smoke_load: the artifact did not serve a local lake:" >&2; echo "$out" >&2; exit 1; }
grep -q '^sum=3$' <<<"$out" || { echo "smoke_load: lake round trip wrong:" >&2; echo "$out" >&2; exit 1; }

# 2. without mssql the gate refuses with our message (autoload pinned off so a repo can't satisfy it)
neg="$("$duckdb_abs" -unsigned -csv -noheader -c "
SET autoload_known_extensions = false;
LOAD '$tmp/mssql_ducklake.duckdb_extension';
" 2>&1)" && { echo "smoke_load: LOAD succeeded without mssql - the deps gate is gone" >&2; exit 1; }
grep -q "mssql_ducklake needs the mssql extension" <<<"$neg" || {
	echo "smoke_load: LOAD without mssql failed, but not with the gate's message:" >&2
	echo "$neg" >&2
	exit 1
}

# 3. the stock-ducklake exclusion; INSTALL needs the official repository, so degrade to a note offline
if "$duckdb_abs" -csv -noheader -c "INSTALL ducklake;" >/dev/null 2>&1; then
	col="$("$duckdb_abs" -unsigned -csv -noheader -c "
LOAD ducklake;
LOAD '$tmp/mssql.duckdb_extension';
LOAD '$tmp/mssql_ducklake.duckdb_extension';
" 2>&1)" && { echo "smoke_load: LOAD succeeded beside stock ducklake - the exclusion gate is gone" >&2; exit 1; }
	grep -q "cannot be loaded together with the ducklake extension" <<<"$col" || {
		echo "smoke_load: LOAD beside stock ducklake failed, but not with the exclusion message:" >&2
		echo "$col" >&2
		exit 1
	}
	exclusion="exclusion gate holds"
else
	exclusion="exclusion gate NOT checked (INSTALL ducklake unavailable - offline?)"
fi

echo "smoke_load: ok (lake sum=3; deps gate refuses without mssql; $exclusion; size $(wc -c <"$ext_abs" | tr -d ' ') bytes)"
