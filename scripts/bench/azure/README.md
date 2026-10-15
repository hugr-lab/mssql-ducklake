# The Azure bench (specs/021)

Postgres and SQL Server DuckLake catalogs on equal VMs in one Azure region, both lakes' parquet in
one storage account, and the bench running beside them in Container Apps. Everything lives in one
resource group, created for the run and deleted after it.

## Before the first run

1. **The image**, built on GitHub rather than on a paid build agent. A push to a `bench/` branch
   builds its head. Once the workflow is on `main`, any commit can be built by hand:

   ```sh
   git push origin bench/<name>                       # or: gh workflow run bench-image.yml -f ref=<commit>
   # -> ghcr.io/hugr-lab/mssql-ducklake-bench:<sha12>
   ```

   The package is public, so Container Apps pulls it without a credential. It holds our binaries
   and scripts, nothing secret.

2. **Azure**: `az login`, and the subscription the run should bill (`az account set -s ...`).

## A run

```sh
BENCH_REF=<commit> scripts/bench/azure/up.sh       # ~15 min; prints nothing secret
scripts/bench/azure/run.sh info                     # the parity check: both servers' versions, CPUs, memory
scripts/bench/azure/run.sh scale --tables 1000      # repeat 3x; the backends' order alternates by run
scripts/bench/azure/run.sh compare --repeat 3
scripts/bench/azure/run.sh concurrent --threads 1,2,4,8,16
scripts/bench/azure/down.sh                         # deletes the resource group, then the local secrets
```

Each `run.sh` waits for its execution and downloads the result to
`scripts/bench/azure/results/<rg>/<execution>/` (gitignored). The managed arm (Azure SQL Database
and PostgreSQL Flexible Server, ~$1.1/h more) is `BENCH_MANAGED=1 up.sh`, and then
`BENCH_TARGET=managed run.sh ...`.

## Settings

All of them are in `config.sh`, and every one can be overridden from the environment:

- `BENCH_LOCATION` (westeurope);
- `BENCH_VM_SIZE` (Standard_D4s_v5);
- `BENCH_SERVER_MEMORY_MB` (12288 for both servers);
- `BENCH_DATA`: `blob` uses `abfss://` and the azure extension; `files` mounts an Azure Files share,
  for when the azure extension does not build on the pinned duckdb;
- `BENCH_BUDGET_USD` (20).

## What it costs, and the guard rails

About $1.5 an hour for the main arm (two D4s_v5 VMs, a 4 vCPU Container Apps profile, a NAT gateway,
storage), so an evening of runs comes to about $6. specs/021 D5 has the table.

- **Budget alert.** The resource group has one at `BENCH_BUDGET_USD`, mailed to the signed-in
  account at 50% and 100%.
- **Teardown.** `down.sh` deletes the whole group and waits until Azure confirms it is gone.
  Forgetting it costs about $36 a day.

## Secrets

The SQL Server and postgres passwords are generated per resource group into `.state/<rg>/` (mode
600, gitignored). They reach the VMs through cloud-init and the job as Container Apps secrets.
Nothing prints them, and the entry point masks any password before it uploads a result. The servers
have no public IP: their NSG admits 1433 and 5432 from the Container Apps subnet only.
