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

Commits are at or near parity: a file-backed commit is one round trip — the manager sends the
commit batch as its own T-SQL, statement by known statement ([Writing](./writing.md#how-a-commit-reaches-the-server))
— and the compaction that rewrites many files' metadata is six times faster than on PostgreSQL.
What remains: the first inlined write into a table (the user's rows go through DuckDB's path, and
the table is touched for the first time), commits of many files (their rows go through DuckDB's
appender per table; the mssql extension's bulk-load insert on the DuckDB 2.0 line changes that),
the flush of many tables' inlined data, and the attach itself — 0.8 s on a 1000-table catalog,
three TDS logins.

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

- **A dedicated database for the catalog.** The one database-wide setting the shaping applies is
  then nobody else's concern, and the plan cache is the catalog's own.
- **Measure your own workload** with the DuckLake metadata log: every metadata query with its
  time is in `duckdb_logs()` after
  `SET enable_logging = true; SET logging_level = 'debug'; SET enabled_log_types = 'DuckLakeMetadata';`
  (with `SET logging_storage = 'memory'` in the CLI), and `make metadata-log WORKLOAD=file.sql`
  aggregates a workload by query shape and the path it took.
