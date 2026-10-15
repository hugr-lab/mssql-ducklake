# Spec 021: postgres and SQL Server catalogs, side by side in Azure

- **Status**: implemented; first run 2026-10-15 (results below)
- **Date**: 2026-10-15
- **Author**: vgsml, with Claude

## Summary

Before the release on the DuckDB 2.0 line, measure the same DuckLake workloads against a postgres
catalog and a SQL Server catalog that run on equal hardware in one Azure region. Both lakes keep
their parquet in one storage account, and the bench runs next to them in Azure Container Apps.

Everything is created for the run, in one resource group, and deleted afterwards. The numbers,
method and caveats become a post on Medium and LinkedIn. The scripts stay in the repository so
anyone can repeat the run.

## Problem

All our numbers so far come from a laptop that runs both servers in Docker. The machine is shared
with other builds: load averages from 9 to 200 were seen during the spec 020 measurements, and the
same arm moved by 30% between runs. Both servers also share one CPU budget with the client. Those
numbers are good for A/B within one build, but not for a public claim like "SQL Server catalog vs
postgres catalog". For that, each server needs its own machine of the same size, the client needs
its own machine, there must be a network between them, and the storage must be what a lake really
uses: object storage, not a local disk.

## Design

### D1: what is compared

The main arm uses equal hardware: two identical VMs (Linux, `Standard_D4s_v6`, 4 vCPU, 16 GiB,
Premium SSD).
- One runs SQL Server 2025 (Developer edition, free license) in the pinned image from
  `docker/docker-compose.yml`.
- The other runs PostgreSQL 18, given the settings a 12 GB, 4 CPU server would get (pgtune's OLTP
  profile). SQL Server sizes its own memory, so leaving postgres at its 128 MB defaults would
  compare a tuned server with an untuned one.
- Both run in containers with the same memory limit.
- The server settings are the ones the repository's compose file uses, with nothing tuned for one
  side only.

This is the fair comparison the post makes: the same CPU, memory and disk.

An optional managed arm compares what a user actually buys: Azure SQL Database (General Purpose,
4 vCores) against Azure Database for PostgreSQL Flexible Server (General Purpose, 4 vCores). These
services are not the same size (Azure SQL GP 4 vCores has about 20 GB of memory, a D4ds_v5
Flexible Server has 16 GB), so this arm is reported apart and labelled as such. It also checks what
the extension does on Azure SQL:
- `PARAMETERIZATION FORCED` (specs/012);
- the UTF-8 collation (specs/006);
- `READ_COMMITTED_SNAPSHOT`, which is on by default there and which specs/019 measured as +8% on the
  scale bench and much better for concurrency.

The owner decides whether the managed arm is run. It costs more.

### D2: one storage, one region

- **One region** for every resource (swedencentral: westeurope refused new resources for this
  subscription, and a new subscription's vCPU quota there has room for Dsv6 but none for Dsv5). Metadata
  round trips dominate the metadata path, so the client and the servers must be close.
- **One storage account**, StorageV2 with the hierarchical namespace (ADLS Gen2), Standard LRS.
- **One container, `lakes`**, with a folder per job execution and per backend inside it. The two
  lakes never share files, and each one's size can be reported. Results go to a `results`
  container.
- **Access** through DuckDB's `azure` extension (`abfss://`) with a SAS or the job's managed
  identity. The key is never written into an image or a log.

### D3: the runner, in Container Apps

- **One image, built on GitHub, not in Azure.** `.github/workflows/bench-image.yml` is run by hand
  for a commit. It runs ci.yml's linux_amd64 build with the `azure` extension added
  (`MSSQL_DUCKLAKE_BENCH_AZURE`) and stages the result in the repository's layout: the `duckdb` CLI,
  `libduckdb`, `mssql_ducklake`, `mssql`, `postgres_scanner`, `azure`, `scripts/bench/`. A
  runtime-only image (`scripts/bench/azure/Dockerfile`) is pushed to GHCR as
  `ghcr.io/hugr-lab/mssql-ducklake-bench:<sha12>`. The package is public, so Container Apps pulls
  it without a credential. No paid build agent and no registry in the resource group.
- **One Container Apps job**, triggered by hand, on a dedicated workload profile with 4 vCPU, so the
  client is not the bottleneck and is the same for both backends.
- **Several replicas for the concurrent bench.** Container Apps can start N replicas of a job. That
  is the multi-node case specs/007, 016 and 015 R3 were written for: separate DuckDB processes on
  separate machines, writing one catalog. On a laptop it could only be simulated.
- **Secrets** are Container Apps secrets, or Key Vault references. The DSNs reach the bench as
  environment variables, as they do in `make bench`. Nothing prints them: the bench scripts already
  mask the DSN in their output.

### D4: the workloads

Each runs against both backends. The order of the backends alternates between runs. Each workload
runs at least three times, and the median is reported with the spread.

1. `make bench-scale BENCH_SCALE_ARGS='--tables 1000'`: the production-shaped catalog, by phase
   (create, commits, flush, compaction, deep history, schema evolution, reattach, time travel).
2. `make bench` (`compare_backends.py`): the basic comparison specs/004 states its target
   against.
3. `concurrent_bench.py`: own and shared tables, readers, mixed; 1 to 16 writers. First in one
   replica; across several replicas once the multi-node workload exists (Follow-ups).
4. A read workload on the parquet in blob storage: filtered reads, partition pruning, time travel.
   This shows what the catalog costs when the data is remote.

Server-side numbers come from the servers themselves (`sys.dm_exec_query_stats`,
`pg_stat_statements`) and from Azure Monitor (CPU, IO) for the run's window.

### D5: cost and teardown

- **One resource group**, `mssql-ducklake-bench-<yyyymmdd>`. Every resource is named
  `mssql-ducklake-*` (the naming rule for local Docker resources too) and tagged
  `purpose=bench`, `owner=vgsml`.
- **A budget alert** on the resource group, created first.
- **Teardown** is one command, `az group delete`, run as the last step of the script and again by
  hand to check. A run that fails mid-way deletes the group too.
- **Cost estimate**, from the Azure Retail Prices API (westeurope, pay-as-you-go, USD, read
  2026-10-15; to be checked again before anything is created):

  | Resource | List price | Per hour |
  | --- | --- | ---: |
  | 2 × VM `D4s_v5` (Linux) | $0.230/h each | $0.46 |
  | 2 × Premium SSD P10 | $21.68/month each | $0.06 |
  | Container Apps environment | $0.143/h | $0.14 |
  | Container Apps dedicated profile, D4 (4 vCPU, 16 GiB) | $0.081/vCPU-h + $0.0066/GiB-h + $0.10/h plan | $0.53 |
  | Extra job replicas for the concurrent bench, consumption | ~$0.09/vCPU-h | ~$0.35 for 4 × 1 vCPU |
  | NAT gateway, storage account, Log Analytics | $0.045/h, cents | ~$0.05 |
  | **Main arm, total** | | **~$1.5/h** |
  | Managed arm (optional): Azure SQL Database GP Gen5 4 vCore | $0.670/h | $0.67 |
  | Managed arm (optional): PostgreSQL Flexible GP Ddsv5 4 vCore (+ storage) | $0.424/h | ~$0.45 |
  | **Managed arm, total** | | **~$1.1/h** |

  An evening of runs (about 4 hours of resources) comes to **~$6 for the main arm** and **~$11
  with the managed arm**. The risk is forgetting the teardown: the main arm costs ~$36 a day. That
  is what the budget alert (set at $20) and the teardown in the script are for.

Nothing is created until the owner has seen the plan, the region and the estimate, and said go.

### D6: the post

The draft lives beside this spec. It is published by the owner and covers:
- what DuckLake's catalog does;
- why SQL Server;
- the setup (D1–D3) with its sizes;
- the method (D4: medians, alternation, server-side numbers);
- the results per phase and the concurrent curves;
- what is faster where and why (round trips, the server-side commit, the one-statement catalog load
  of specs/020);
- the caveats: dev DuckDB, ducklake at a main commit, one region, Developer edition rather than a
  licensed one, the managed arm not equal in memory;
- how to reproduce the run.

## Results (2026-10-15)

**Setup as run.**
- Region: swedencentral.
- Catalog servers: two `Standard_D4s_v6` VMs (4 vCPU, 16 GiB, NVMe Premium SSD):
  - SQL Server 2025 CU8, `MSSQL_MEMORY_LIMIT_MB=12288`;
  - PostgreSQL 18.6 with `shared_buffers=3GB` and `effective_cache_size=9GB`.
- Runner: a Container Apps job on a D4 workload profile (3.5 vCPU, 14 GiB).
- Lake data: an Azure Files share in the same storage account (`BENCH_DATA=files`; see the
  findings for why not blob).
- Build: mssql-ducklake 9e308d2 (specs/020 phases A and B), duckdb bde1b27, ducklake b0d3643a,
  mssql 412c63f (the same code as 0.3.0's ef1a807).

**`scale --tables 500`, three runs** (backend order mssql/postgres, postgres/mssql, mssql/postgres).
Median, with min-max:

| phase | SQL Server | PostgreSQL | ratio |
| --- | ---: | ---: | ---: |
| `create_tables` | 71.9 s (70-73) | 115.0 s (114-124) | 0.63x |
| `first_commits` | 26.4 s (26-28) | 33.8 s (33-35) | 0.78x |
| `second_commits` | 46.1 s (46-49) | 76.5 s (76-82) | 0.60x |
| `flush_inlined` | 32.5 s (32-33) | 80.5 s (80-84) | 0.40x |
| `merge_adjacent` | 30.3 s (29-32) | 31.9 s (31-35) | 0.95x |
| `partitioned_commits` | 60.6 s (59-65) | 67.7 s (63-72) | 0.90x |
| `partitioned_merge_adjacent` | 30.9 s (30-32) | 66.9 s (66-71) | 0.46x |
| `deep_history` | 385.7 s (361-431) | 598.9 s (565-608) | 0.64x |
| `evolution` | 54.0 s (51-54) | 97.0 s (90-98) | 0.56x |
| `evolution_read_latest` | 7.6 s (7-8) | 17.3 s (17-19) | 0.44x |
| reads, time travel, reattach | | | 0.8-1.2x |
| **total** | **759.6 s (724-817)** | **1199.6 s (1157-1242)** | **0.63x** |

The order of the backends does not move the result. The phases where SQL Server wins by the most
are the catalog's own work: the commits, the flush, the compactions and the schema evolution,
where specs/014's commit batch and specs/020's catalog load do their part. Where the parquet writes
dominate, as in `merge_adjacent` and `partitioned_commits`, the two are even.

**`concurrent`** (20 operations per thread; `own_inlined`, `shared_file` and `mixed` at 1, 4 and
16 threads):
- SQL Server finished all 12 configurations.
- PostgreSQL failed 2 of them, both at 16 writers: `own_inlined` and `shared_file`, each ending in
  "Failed to commit DuckLake". Its retries ran out.
- With 16 threads on `mixed`, SQL Server ran 19.9 operations a second against 15.7.

**`compare`** did not run: `compare_backends.py` lacked the autoload switch-off that
`scale_catalog.py` has, and the 2.0 line ends that session. Fixed afterwards. `scale` covers the
same ground at more scale.

**Cost.** About four and a half hours of resources, roughly $7 at list prices.

## Findings on the way

- **DuckLake's flush needs memory that grows about quadratically with the number of tables.**
  Measured locally as the peak RSS of a flush:

  | tables | peak RSS |
  | ---: | ---: |
  | 150 | 973 MB |
  | 300 | 3024 MB |

  It is the same with the mirror of specs/020 as without (3024 against 3019 MB). The likely
  mechanism: the flush transaction pins a full catalog, every table in it, for each schema version
  it flushes. Extrapolated, 1000 tables needs 20-30 GB. That is why the runner, at 14 GiB, was
  OOM-killed in the 1000-table flush on blob and on Azure Files alike, and why the bench ran at 500.
  Postgres catalogs pay it the same way. An upstream issue, still to be raised.
- **The azure extension's credential chain does not find a user-assigned identity.** Its `default`
  chain builds `DefaultAzureCredential` with default options, which reads no `AZURE_CLIENT_ID`.
  `PROVIDER managed_identity` with `CLIENT_ID` does find it.
- **Blob is slower than Azure Files for this bench's writes.** The bench writes thousands of small
  files, one per commit, and `deep_history` alone took 370 s at 200 tables on blob.
- **`az containerapp job start --args` reads any value that starts with a dash as its own
  option.** `run.sh` therefore starts executions through the REST API, with the container spelled
  out (image, environment with secret references, resources, volume mounts).
- **Running unattended.** The tail of the run was one job execution: a shell queue that waited for
  the running execution through the job's identity and the ARM API, ran the remaining workloads,
  copied the results to a storage account in a separate resource group, and deleted the bench's
  resource group itself (the identity held Contributor on that group only). No laptop was needed.

## Enforcement & security

- **Credentials.** No secret is written into the repository, an image, a log or the post. Server
  passwords are generated per run and live only as Container Apps secrets and in the VMs'
  environment. They die with the resource group.
- **Network.** The servers accept connections only from the Container Apps environment's subnet (a
  VNet with NSG rules) and from nowhere else. No public 1433 or 5432.
- **Spending.** Everything is in one resource group, with a budget alert and an explicit teardown,
  and nothing runs unattended overnight.

## Testing

- **A dry run first**: the same scripts against the local Docker servers with `--tables 50`. Then
  one small run in Azure (`--tables 50`) that checks the wiring before the full runs.
- **The parity check**: before the full runs, `SELECT @@VERSION` / `version()`, the CPU count and the
  memory each server sees are recorded, and the run is refused if the two VMs differ.
- **Every result table carries** the build's SHAs (duckdb, ducklake, mssql, this repo), the VM sizes
  and the run's date.

## Alternatives considered

- **Bench from the laptop against Azure servers.** The round trips would cross the internet and
  the result would measure the ISP.
- **One VM running both servers.** Cheaper, but the two share a CPU, which is what is wrong with
  the laptop numbers.
- **AKS instead of Container Apps.** More to set up and to tear down, for nothing the bench needs.
- **Managed services only.** They are not the same size, so they cannot make the "equal hardware"
  claim. They stay as the optional second arm.

## Follow-ups

- **The extensions in the image.** `azure` and `postgres_scanner` for a DuckDB development commit are
  not published, so they are built in the same tree as `mssql_ducklake` (`MSSQL_DUCKLAKE_BENCH_AZURE`
  in `extension_config.cmake`, bench builds only). After `v2.0.0` they can come from the extension
  repository instead. If `azure` does not build on the pin, `BENCH_DATA=files` puts the lakes on an
  Azure Files share mounted into the job.
- **Several nodes against one catalog.** The concurrent bench resets its schema in every run, so
  replicas of one execution would reset each other. A multi-node workload, where one replica sets up
  and N replicas write, is still to be written. The infrastructure supports it: the workload profile
  scales to `BENCH_PROFILE_MAX_NODES`.
- **The infrastructure** lives in `scripts/bench/azure/` (`up.sh`, `run.sh`, `down.sh`, cloud-init
  for the two servers, the job's entry point), so the post can link to it.
