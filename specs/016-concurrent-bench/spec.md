# Spec 016: many writers and readers through one DuckDB

- **Status**: draft (measured; the read scaling traced to mssql-extension#409)
- **Date**: 2026-10-06
- **Author**: vgsml, with Claude

## Summary

The scale bench (`make bench-scale`) measures one user: every phase is serial, one DuckLake
transaction at a time on one connection, so each server works on one core. This adds the other
shape - N threads of ONE DuckDB process, a connection each, writing and reading the same lake at
once - and measures it on both backends and both DuckDB lines.

## Problem

Nothing measured concurrency. `make test-concurrent` (specs/007) is a correctness test: processes
committing into their own tables, checked for lost writers, timed by nobody.

## Design

- `scripts/bench/concurrent_harness.c`: a few hundred lines on DuckDB's C API, compiled against the
  build's own `libduckdb` (the CLI has one connection; no Python package matches a dev build), so
  the same source measures any line. One database, one `ATTACH` of the lake, a thread per
  connection, a SQL template per role (`{t}` the thread, `{i}` the iteration). Connection strings
  come from the environment, never a file.
- `scripts/bench/concurrent_bench.py` (`make bench-concurrent`): the scenarios, each from an empty
  lake in the metadata schema `bench_conc`; every run checks that the writers' rows are all there.
  - `own_inlined` / `own_file`: N writers, each into its own table, 2 rows (inlined) / 200 rows (a
    data file) a commit;
  - `shared_file`: N writers into one table;
  - `read`: N readers, filtered reads over 16 pre-filled tables (5 files each);
  - `mixed`: N/2 `own_file` writers and N/2 readers.

## Measured (2026-10-06)

20 operations a thread, N = 1, 2, 4, 8, 16, the bench tables' width (id + 40 columns). DuckDB 1.5.6
is `main` (v0.1.2 + mssql v0.2.5, postgres from the extension repository); 2.0 is this branch.
Throughput is the role's operations over its wall time; at N = 1 that includes the first, cold
operation, and with 20 operations p95 is close to the maximum.

`own_inlined`, writers — ops/s / p95 ms (✗: a writer failed)

| N | mssql 1.5.6 | mssql 2.0 | pg 1.5.6 | pg 2.0 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 47 / 94 | 35 / 145 | 143 / 19 | 74 / 38 |
| 2 | 37 / 292 | 18 / 909 | 118 / 136 | 56 / 46 |
| 4 | 44 / 374 | 23 / 982 | 129 / 102 | 60 / 318 |
| 8 | 38 / 734 | 26 / 991 | 95 / 246 | 54 / 665 |
| 16 | 39 / 1552 | 25 / 2127 ✗ | 78 / 323 | 48 / 1194 |

`own_file`, writers — ops/s / p95 ms (✗: a writer failed)

| N | mssql 1.5.6 | mssql 2.0 | pg 1.5.6 | pg 2.0 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 49 / 100 | 39 / 133 | 113 / 21 | 66 / 43 |
| 2 | 39 / 267 | 31 / 609 | 111 / 106 | 49 / 228 |
| 4 | 44 / 541 | 29 / 542 | 98 / 196 | 56 / 305 |
| 8 | 42 / 812 | 33 / 797 | 74 / 281 | 56 / 697 |
| 16 | 38 / 1385 ✗ | 24 / 2066 ✗ | 98 / 563 | 38 / 1143 |

`shared_file`, writers — ops/s / p95 ms (✗: a writer failed)

| N | mssql 1.5.6 | mssql 2.0 | pg 1.5.6 | pg 2.0 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 52 / 70 | 37 / 114 | 114 / 21 | 72 / 36 |
| 2 | 42 / 263 | 31 / 294 | 103 / 22 | 62 / 38 |
| 4 | 53 / 467 | 39 / 309 | 99 / 118 | 64 / 235 |
| 8 | 51 / 719 | 27 / 947 | 82 / 141 | 55 / 470 |
| 16 | 46 / 1415 | 27 / 2629 | 84 / 476 | 49 / 785 |

`read`, readers — ops/s / p95 ms (✗: a writer failed)

| N | mssql 1.5.6 | mssql 2.0 | pg 1.5.6 | pg 2.0 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 85 / 93 | 46 / 170 | 126 / 25 | 98 / 32 |
| 2 | 91 / 292 | 74 / 282 | 226 / 26 | 178 / 38 |
| 4 | 115 / 371 | 78 / 580 | 332 / 30 | 297 / 45 |
| 8 | 130 / 534 | 69 / 1541 | 487 / 32 | 447 / 48 |
| 16 | 188 / 607 | 70 / 2744 | 635 / 60 | 618 / 72 |

`mixed`, writers — ops/s / p95 ms (✗: a writer failed)

| N | mssql 1.5.6 | mssql 2.0 | pg 1.5.6 | pg 2.0 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 62 / 33 | 36 / 80 | 114 / 15 | 63 / 24 |
| 2 | 40 / 190 | 37 / 82 | 122 / 14 | 64 / 21 |
| 4 | 57 / 214 | 24 / 426 | 94 / 23 | 60 / 24 |
| 8 | 47 / 430 | 23 / 1151 | 88 / 197 | 49 / 322 |
| 16 | 47 / 681 | 14 / 2988 | 60 / 376 | 44 / 719 |

`mixed`, readers — ops/s / p95 ms (✗: a writer failed)

| N | mssql 1.5.6 | mssql 2.0 | pg 1.5.6 | pg 2.0 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 44 / 289 | 29 / 303 | 95 / 26 | 63 / 40 |
| 2 | 38 / 283 | 34 / 177 | 103 / 26 | 64 / 35 |
| 4 | 57 / 305 | 24 / 715 | 94 / 26 | 60 / 40 |
| 8 | 47 / 511 | 23 / 1469 | 88 / 34 | 49 / 70 |
| 16 | 47 / 859 | 14 / 4469 | 60 / 57 | 44 / 73 |

## What it shows

1. **Reads do not scale on SQL Server — found: the mssql extension's catalog-wide lock.** postgres
   goes from ~100 to ~620 reads a second as readers go from 1 to 16, its p95 under 75 ms. SQL
   Server: 85 → 188 on 1.5.6, and on 2.0 no scaling at all (46 → 70, p95 2.7 s). During 16 readers
   the server is idle (0–1 running requests on 17 sessions, sampled every 0.5 s) and the DuckDB
   process uses ~1.5 cores; `sample` of the worker threads: **85.5% parked in `std::mutex::lock`
   under `MSSQLScanInitGlobal`**, 6.4% in the one thread holding it, reading the socket. The
   mutex is `MSSQLCatalog::materialize_mutex_`, one per attached catalog: a materialized scan holds
   it from its batch through its drain, and since spec 081 every raw scan inside a transaction is
   one. Its comment assumes the contenders share one pinned connection — true for one DuckDB
   connection, false for N, each with its own. On 1.5.6 (mssql v0.2.5) only catalog scans took it,
   hence the regression: our metadata reads moved to raw scans on 2.0. Filed as
   hugr-lab/mssql-extension#409 (the lock per pinned connection); the fix is the extension's.
   Writers pay it too: every commit's metadata reads are in-transaction scans.
2. **Mixed: writers drag readers down on SQL Server, not on postgres.** postgres readers keep p95
   ~70 ms beside 8 writers; SQL Server readers go to 1.5–4.5 s. A candidate: under `READ
   COMMITTED` without row versioning, a reader of `ducklake_snapshot` waits on a writer's
   uncommitted row; postgres' MVCC readers never wait. `READ_COMMITTED_SNAPSHOT` on the catalog's
   database would remove that - a database option, like specs/012's. **To be measured.**
3. **Commits serialise on every backend** - DuckLake's single snapshot sequence - so writers' total
   throughput stays flat as N grows; the per-commit cost sets the ceiling. SQL Server's ceiling is
   lower and its retries run out: at 16 writers some commits fail with *Exceeded the maximum
   retry count of 10*, on both lines; postgres never did.
4. **2.0 against 1.5.6**: both backends lose at concurrency on 2.0 (postgres's single-connection
   numbers roughly halve too, as in the scale bench) - partly DuckDB's line, partly ours to find.

## Follow-ups

- ~~Find what serialises SQL Server readers~~ — mssql-extension#409; re-measure with its fix.
- At the first touch all 16 sessions wait on `RESOURCE_SEMAPHORE_QUERY_COMPILE` (the cold tail
  of ~4 s): 16 compiles at once of the same statements.
- Measure `READ_COMMITTED_SNAPSHOT` on the catalog's database for the mixed and writer scenarios.
- The retry exhaustion at 16 writers: how long a failed attempt holds the snapshot, and whether
  `ducklake_max_retry_count` or the backoff is what differs from postgres.
