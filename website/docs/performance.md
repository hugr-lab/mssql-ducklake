---
title: Performance
sidebar_position: 4
---

# Performance

The target is the PostgreSQL backend: a DuckLake catalog in SQL Server should cost no more than
one in PostgreSQL. It does not yet, and this page says by how much and why. Every number is from
`make bench-scale --tables 1000` — a catalog of 1000 tables with 40 columns each across 10
schemas, 20 partitioned tables, three tables with 1000 snapshots, one with 100 schema versions —
run against SQL Server 2025 and PostgreSQL 18 in Docker on the same machine, one statement per
phase, cold.

### Where it stands

| phase | SQL Server | PostgreSQL | ratio |
| --- | ---: | ---: | ---: |
| create 1000 tables | 160 s | 104 s | 1.5x |
| first insert into each (inlined) | 29 s | 5.8 s | 5.1x |
| second insert into each (a data file) | 25 s | 9.5 s | 2.7x |
| 100 partitioned commits of 25 files | 32 s | 6.2 s | 5.2x |
| 1000 commits into one table | 78 s | 32 s | 2.5x |
| flush 1000 tables' inlined data | 248 s | 154 s | 1.6x |
| re-attach | 0.79 s | 0.13 s | 6.2x |
| first filtered read after an attach | 0.45 s | 0.18 s | 2.5x |
| read at an old snapshot, 1000 snapshots deep | 0.02 s | 0.01 s | 2.0x |
| read a table with 100 schema versions | 14 s | 8.3 s | 1.7x |
| **whole benchmark** | **698 s** | **388 s** | **1.80x** |

Reads are close: a warm read costs the same on both, the first read after an attach is a quarter
of a second more, and an attach itself is 0.8 s on a 1000-table catalog — most of it the mssql
extension discovering the schema. **Writes are the gap**: a commit is ~19 round trips through
DuckDB's DML operators against the attached catalog, and the PostgreSQL manager sends its batch in
one call. A server-side commit that does the same is the next piece of work; its experimental
first form is [behind a switch](./writing.md#the-server-side-commit-experimental).

### What made the difference so far

- **Keys, indexes, collation** ([Shaping](./catalog/shaping.md)): the per-column statistics index
  turned a filtered read's pruning query from 15 ms into 1 ms; the partition index does the same for
  partition pruning; the visibility index serves time travel as well as it serves the current
  state.
- **Forced parameterization**: `PARAMETERIZATION FORCED` on the catalog's database took the
  benchmark from 937 s to 698 s. Without it every distinct literal — a table name in the mssql
  extension's metadata query, a `table_id` in DuckLake's — is an ad-hoc plan compiled on first use,
  ~35 ms each; on this server that meant every first touch of every table. It is applied by the
  shaping and can be [opted out of](./reference/settings.md).
- **The read layer in T-SQL**: the one lookup a read makes that could miss — does this table have
  an inlined-deletes table — goes to the server as one `mssql_scan()` statement; through the
  catalog path a miss made the mssql extension reload the schema's metadata, a second on a
  1000-table catalog. The other lookups deliberately stay on the catalog path: they warm the
  metadata cache for exactly the tables the commit then updates.
- **The conflict check as one statement**: correctness first (see
  [concurrent writers](./writing.md#concurrent-writers)), but also 14 batches instead of 25 on a
  retry.

### Advice

- **A dedicated database for the catalog.** The one database-wide setting the shaping applies is
  then nobody else's concern, and the plan cache is the catalog's own.
- **Measure your own workload** with the DuckLake metadata log: every metadata query with its
  time is in `duckdb_logs()` after
  `SET enable_logging = true; SET logging_level = 'debug'; SET enabled_log_types = 'DuckLakeMetadata';`
  (with `SET logging_storage = 'memory'` in the CLI), and `make metadata-log WORKLOAD=file.sql`
  aggregates a workload by query shape and the path it took.
