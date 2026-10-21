---
title: Performance
sidebar_position: 4
---

# Performance

The target is the PostgreSQL backend: a DuckLake catalog in SQL Server should cost no more than
one in PostgreSQL. On v0.2.0 it costs less on the benchmark below. This page gives the numbers and
explains what got them there.

### Where it stands

**Equal Azure VMs**, v0.2.0's code on pre-release builds of DuckDB 2.0, as of 2026-10-15.

- **Servers:** SQL Server 2025 and PostgreSQL 18, each on a `Standard_D4s_v6` VM (4 vCPU, 16 GiB), both tuned for their memory.
- **Client:** DuckDB with this extension, as an Azure Container Apps job in the same VNet.
- **Data files:** Parquet in one shared storage account.
- **Workload:** `make bench-scale` at 500 tables of 41 columns across 10 schemas, one statement per phase.
- **Runs:** three, with the order of the backends alternated. Each figure is the median, with the range in brackets.

| phase | SQL Server | PostgreSQL | ratio |
| --- | ---: | ---: | ---: |
| create 500 tables | 71.9 s (70–73) | 115.0 s (114–124) | 0.63× |
| first insert into each (inlined) | 26.4 s (26–28) | 33.8 s (33–35) | 0.78× |
| second insert into each (Parquet) | 46.1 s (46–49) | 76.5 s (76–82) | 0.60× |
| flush inlined data | 32.5 s (32–33) | 80.5 s (80–84) | 0.40× |
| compaction | 30.3 s (29–32) | 31.9 s (31–35) | 0.95× |
| partitioned commits | 60.6 s (59–65) | 67.7 s (63–72) | 0.90× |
| partitioned compaction | 30.9 s (30–32) | 66.9 s (66–71) | 0.46× |
| 3000 commits into 3 tables | 385.7 s (361–431) | 598.9 s (565–608) | 0.64× |
| 100 schema changes | 54.0 s (51–54) | 97.0 s (90–98) | 0.56× |
| read after 100 schema changes | 7.6 s (7–8) | 17.3 s (17–19) | 0.44× |
| **whole benchmark** | **759.6 s** | **1199.6 s** | **0.63×** |

SQL Server wins where the catalog does the work and ties where the Parquet does. The limits of the
measurement:

- one region and one VM size;
- self-managed servers, not Azure SQL or Azure Database for PostgreSQL;
- builds from before the releases.

The same benchmark at 1000 tables in Docker on one machine took ~420 s on SQL Server and ~1100 s
on PostgreSQL that day (specs/020).

### v0.1.x, for comparison

On DuckDB v1.5.6, `make bench-scale --tables 1000` in Docker on one machine, cold:

| phase | SQL Server | PostgreSQL | ratio |
| --- | ---: | ---: | ---: |
| create 1000 tables | 150 s | 98 s | 1.5x |
| first insert into each (inlined) | 16.7 s | 5.8 s | 2.9x |
| second insert into each (a data file) | 11.4 s | 9.0 s | 1.3x |
| 100 partitioned commits of 25 files | 30.7 s | 6.1 s | 5.0x |
| 1000 commits into one table | 35 s | 30 s | 1.2x |
| merge adjacent files, 1000 tables | 3.4 s | 22.4 s | 0.15x |
| flush 1000 tables' inlined data | 240 s | 150 s | 1.6x |
| re-attach | 0.82 s | 0.13 s | 6.4x |
| first filtered read after an attach | 0.37 s | 0.18 s | 2.1x |
| read at an old snapshot, 1000 snapshots deep | 0.02 s | 0.01 s | 2.0x |
| read a table with 100 schema versions | 13.7 s | 7.5 s | 1.8x |
| **whole benchmark** | **577 s** | **375 s** | **1.54x** |


### What made the difference so far

On the DuckDB 2.0 line, v0.2.0:

- **The catalog load in one T-SQL statement, then a mirror per ATTACH** (specs/020). DuckLake
  rebuilds its catalog many times: on every DDL commit, and once per table in a flush. Each rebuild
  was seven queries over the whole catalog. Now it is one statement the first time, and after that
  only the rows that changed since the mirror's snapshot. On the 1000-table bench this took the
  flush from 171 s to 79 s and the whole run from 650 s to 420 s.
- **Table statistics cached per ATTACH** (specs/019), and refreshed from the commits that change
  them.
- **Bounded strings** (specs/018). The catalog's string columns are `VARCHAR(n)` instead of
  `VARCHAR(MAX)`. The bounds are chosen when the catalog is created or migrated from 1.0, and
  `META_LIMITS` can override them.
- **Asynchronous statistics updates** on the catalog's database (specs/017). A query that finds a
  table's statistics stale runs on the ones it has, and the update happens beside it.
- **The commit with the user's inlined rows in the same T-SQL run**, and the file list of a scan as
  one server statement (specs/015).

From v0.1.x:


- **Keys, indexes, collation** ([Shaping](./catalog/shaping.md)): the per-column statistics index
  turned a filtered read's pruning query from 15 ms into 1 ms; the partition index does the same for
  partition pruning; the visibility index serves time travel as well as it serves the current
  state.
- **Forced parameterization**: `PARAMETERIZATION FORCED` on the catalog's database took the
  benchmark from 937 s to 698 s. Without it every distinct literal — a table name in the mssql
  extension's metadata query, a `table_id` in DuckLake's — is an ad-hoc plan compiled on first use,
  ~35 ms each; on this server that meant every first touch of every table. It is applied by the
  shaping and can be [opted out of](./reference/settings.md).
- **The commit batch in T-SQL**: DuckLake's commit ran statement by statement through DuckDB's
  DML operators — ~19 round trips and a catalog-sized scan for the stats update; recognised
  exactly, from a closed list of the statements DuckLake writes, and sent as the manager's own T-SQL
  in one call, a file-backed commit went from 25.5 ms to 11.4 (PostgreSQL: 9.0), the benchmark from
  698 s to 577.
- **The read layer in T-SQL**: the one lookup a read makes that could miss — does this table have
  an inlined-deletes table — goes to the server as one `mssql_scan()` statement; through the
  catalog path a miss made the mssql extension reload the schema's metadata, a second on a
  1000-table catalog. The other lookups deliberately stay on the catalog path: they warm the
  metadata cache for exactly the tables the commit then updates.
- **The conflict check as one statement**: correctness first (see
  [concurrent writers](./writing.md#concurrent-writers)), but also 14 batches instead of 25 on a
  retry.

### Advice

- **A dedicated database for the catalog.** The two database-wide settings the shaping applies are
  then nobody else's concern, and the plan cache is the catalog's own.
- **Measure your own workload** with the DuckLake metadata log: every metadata query with its
  time is in `duckdb_logs()` after
  `SET enable_logging = true; SET logging_level = 'debug'; SET enabled_log_types = 'DuckLakeMetadata';`
  (with `SET logging_storage = 'memory'` in the CLI), and `make metadata-log WORKLOAD=file.sql`
  aggregates a workload by query shape and the path it took.
