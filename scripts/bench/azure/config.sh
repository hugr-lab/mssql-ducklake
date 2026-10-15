# The Azure bench's settings (specs/021). Sourced by up.sh, run.sh and down.sh; every value can be
# overridden from the environment. Nothing secret lives here: the generated passwords are written to
# the run's state directory (.state/, gitignored, mode 600) and never printed.

: "${BENCH_LOCATION:=swedencentral}"
: "${BENCH_DATE:=$(date +%Y%m%d)}"
: "${BENCH_RG:=mssql-ducklake-bench-${BENCH_DATE}}"
# both catalog servers get this size: the equal hardware the comparison claims. 4 vCPU, 16 GiB;
# v6 because a new subscription's quota has room for Dsv6 and none for Dsv5
: "${BENCH_VM_SIZE:=Standard_D4s_v6}"
# the memory each server may use, in MB, of the VM's 16 GiB
: "${BENCH_SERVER_MEMORY_MB:=12288}"
# the images: the ones docker/docker-compose.yml runs locally
: "${BENCH_MSSQL_IMAGE:=mcr.microsoft.com/mssql/server:2025-CU8-ubuntu-24.04}"
: "${BENCH_PG_IMAGE:=postgres:18}"
# the Container Apps workload profile the bench runs on, and how many nodes it may scale to
: "${BENCH_PROFILE_TYPE:=D4}"
: "${BENCH_PROFILE_MAX_NODES:=8}"
# the commit of hugr-lab/mssql-ducklake whose image runs: bench-image.yml builds it on GitHub and
# pushes ghcr.io/hugr-lab/mssql-ducklake-bench:<sha12>; BENCH_IMAGE names another image outright
: "${BENCH_REF:=main}"
# blob: the lakes' parquet in ADLS Gen2 through the azure extension (abfss://); files: an Azure Files
# share mounted into the job, for when the azure extension does not build on the pinned duckdb
: "${BENCH_DATA:=blob}"
# the optional second arm (specs/021 D1): Azure SQL Database and PostgreSQL Flexible Server
: "${BENCH_MANAGED:=0}"
# the budget alert on the resource group, in USD
: "${BENCH_BUDGET_USD:=20}"

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_STATE="${BENCH_DIR}/.state/${BENCH_RG}"

# Every name is mssql-ducklake-* where Azure allows dashes. A storage account's name is global,
# lowercase alphanumerics only: a suffix from the subscription and the date keeps it unique without
# a random value the other scripts would have to find.
bench_names() {
	local sub
	sub="$(az account show --query id -o tsv)"
	BENCH_SUFFIX="$(printf '%s%s' "$sub" "$BENCH_RG" | shasum | cut -c1-8)"
	BENCH_STORAGE="mssqldl${BENCH_SUFFIX}"
	BENCH_VNET="mssql-ducklake-vnet"
	BENCH_IDENTITY="mssql-ducklake-bench-id"
	BENCH_ENV="mssql-ducklake-env"
	BENCH_JOB="mssql-ducklake-bench"
	BENCH_VM_MSSQL="mssql-ducklake-sqlserver"
	BENCH_VM_PG="mssql-ducklake-postgres"
	BENCH_LOGS="mssql-ducklake-logs"
}

step() {
	printf '\n== %s\n' "$*" >&2
}

die() {
	printf 'error: %s\n' "$*" >&2
	exit 1
}
