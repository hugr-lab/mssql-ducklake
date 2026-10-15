#!/usr/bin/env bash
# The bench job's entry point inside the image (specs/021). One workload per execution:
#
#   info                       the build and what each catalog server reports about itself
#   scale [scale_catalog.py args]
#   compare [compare_backends.py args]
#   concurrent [concurrent_bench.py args]
#
# BENCH_TARGET=managed swaps in the managed arm's servers (Azure SQL Database, PostgreSQL Flexible).
# The output goes to stdout (the job's log) and to the results container, under the execution's
# name. The DSNs arrive as secrets in the environment and are never printed: the bench scripts mask
# passwords in what they print, and this script prints none.
set -euo pipefail
cd /opt/mssql-ducklake

workload="${1:-info}"
shift || true
run_id="${CONTAINER_APP_JOB_EXECUTION_NAME:-local-$(date +%Y%m%d%H%M%S)}"
replica="${CONTAINER_APP_REPLICA_NAME:-0}"
account="${BENCH_STORAGE_ACCOUNT:?BENCH_STORAGE_ACCOUNT is set by up.sh}"

if [[ "${BENCH_TARGET:-vm}" == managed ]]; then
	export MSSQL_DUCKLAKE_TEST_DSN="${BENCH_MANAGED_MSSQL_DSN:?the managed arm was not created (BENCH_MANAGED=1 up.sh)}"
	export MSSQL_DUCKLAKE_PG_DSN="${BENCH_MANAGED_PG_DSN:?the managed arm was not created}"
fi

# where the lakes' parquet goes: blob storage through the azure extension, or the mounted share
case "${BENCH_DATA:-blob}" in
blob)
	export MSSQL_DUCKLAKE_BENCH_DATA_ROOT="abfss://lakes@${account}.dfs.core.windows.net/${run_id}"
	# the job's user-assigned identity: DefaultAzureCredential (CHAIN 'default') picks it by
	# AZURE_CLIENT_ID, which up.sh sets; 'managed_identity' alone asks for a system-assigned one
	export MSSQL_DUCKLAKE_BENCH_PREAMBLE="LOAD 'build/release/extension/azure/azure.duckdb_extension';
CREATE SECRET bench_azure (TYPE azure, PROVIDER credential_chain, CHAIN '${BENCH_AZURE_CHAIN:-default}', ACCOUNT_NAME '${account}');"
	;;
files)
	export MSSQL_DUCKLAKE_BENCH_DATA_ROOT="/mnt/lakes/${run_id}"
	mkdir -p "$MSSQL_DUCKLAKE_BENCH_DATA_ROOT"
	;;
*)
	echo "unknown BENCH_DATA: ${BENCH_DATA}" >&2
	exit 2
	;;
esac

out="/tmp/${workload}-${replica}.txt"
{
	echo "# workload: ${workload} $*"
	echo "# execution: ${run_id} replica ${replica}"
	echo "# target: ${BENCH_TARGET:-vm} (${BENCH_VM_SIZE:-?}), data: ${BENCH_DATA:-blob}"
	echo "# started: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	sed 's/^/# build: /' BUILD_INFO
} >"$out"

info_sql() {
	cat <<'SQL'
SET autoload_known_extensions = false;
LOAD 'build/release/extension/mssql/mssql.duckdb_extension';
LOAD 'build/release/extension/postgres_scanner/postgres_scanner.duckdb_extension';
ATTACH getenv('MSSQL_DUCKLAKE_TEST_DSN') AS srv (TYPE mssql);
SELECT * FROM mssql_scan('srv', 'SELECT CAST(@@VERSION AS NVARCHAR(400)) AS version, cpu_count, CAST(physical_memory_kb / 1024 AS BIGINT) AS memory_mb, CAST(committed_target_kb / 1024 AS BIGINT) AS target_mb FROM sys.dm_os_sys_info');
SELECT * FROM mssql_scan('srv', 'SELECT name, is_read_committed_snapshot_on AS rcsi, is_parameterization_forced AS forced FROM sys.databases WHERE name = DB_NAME()');
ATTACH getenv('MSSQL_DUCKLAKE_PG_DSN') AS pg (TYPE postgres, READ_ONLY);
SELECT * FROM postgres_query('pg', 'SELECT version(), current_setting(''shared_buffers'') AS shared_buffers, current_setting(''effective_cache_size'') AS effective_cache_size, current_setting(''max_connections'') AS max_connections');
SQL
}

status=0
case "$workload" in
info)
	info_sql | build/release/duckdb -unsigned -box >>"$out" 2>&1 || status=$?
	;;
scale)
	python3 scripts/bench/scale_catalog.py "$@" >>"$out" 2>&1 || status=$?
	;;
compare)
	python3 scripts/bench/compare_backends.py "$@" >>"$out" 2>&1 || status=$?
	;;
concurrent)
	python3 scripts/bench/concurrent_bench.py "$@" >>"$out" 2>&1 || status=$?
	;;
*)
	echo "unknown workload: ${workload} (info, scale, compare, concurrent)" >>"$out"
	status=2
	;;
esac
echo "# finished: $(date -u +%Y-%m-%dT%H:%M:%SZ), exit ${status}" >>"$out"

# a password that reached the output by some path nobody foresaw is masked before it leaves
sed -i -E 's/(([Pp]assword|PWD|pwd)[[:space:]]*=[[:space:]]*)[^;[:space:]'"'"']+/\1***/g' "$out"
cat "$out"
export AZCOPY_AUTO_LOGIN_TYPE=MSI AZCOPY_MSI_CLIENT_ID="${AZURE_CLIENT_ID:-}"
azcopy copy "$out" "https://${account}.blob.core.windows.net/results/${run_id}/" --log-level ERROR >/dev/null ||
	echo "warning: the result was not uploaded; it is in the job's log above" >&2
exit "$status"
