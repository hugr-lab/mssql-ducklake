---
title: Releases
sidebar_position: 8
---

# Releases

Versions of this documentation follow the releases: the dropdown shows the docs of each tag, and
*Next* is what is being written for the coming one.

## v0.1.1 — a catalog that takes two tables in one commit

DuckDB **v1.5.5**, DuckLake `v1.5-variegata` embedded, the mssql extension **v0.2.5** or newer.
A fix release: nothing else changed.

**The fix.** A transaction that created or altered **two or more tables** failed to commit, with
`Violation of PRIMARY KEY constraint 'pk_ducklake_schema_versions'` from the server and ten retries
after it. DuckLake records one row per table whose schema changed in a snapshot, and the key this
extension put on that table did not include the table — so the second table of the commit read as a
duplicate. Reported from a Fabric SQL database as
[issue #30](https://github.com/hugr-lab/mssql-ducklake/issues/30); a single-table commit, which is
what the tests and the benchmark did, never met it.

**What it means for an existing catalog.** Nothing to do: attach it with this version and the key
is rebuilt in place, along with anything else the [shaping](./catalog/shaping.md) has corrected
since the catalog was made. That repair is itself new — keys were applied by name, so a catalog
already shaped kept the key it was made with. A catalog fixed by hand in the meantime reads as
correct and is left alone.

**Everything else** is as in v0.1.0 below, including the platforms.

## v0.1.0 — the first release

DuckDB **v1.5.5**, DuckLake `v1.5-variegata` embedded, the mssql extension **v0.2.5** or newer.
Experimental.

**What it does.** A DuckLake catalog in SQL Server or Azure SQL: `ATTACH 'ducklake:mssql:…'`
creates or opens it, and everything DuckLake does works on it — DDL, inlined and file-backed
writes, `UPDATE`, `DELETE`, `MERGE`, schema evolution, partitioning, time travel, the maintenance
functions, several writers at once.

**What the manager adds for SQL Server.** The catalog is [shaped](./catalog/shaping.md) on
creation and brought up to shape on attach: primary keys on every table DuckLake updates, strings
as `VARCHAR` under a UTF-8 binary collation, the indexes DuckLake's reads want, forced
parameterization on the database. The commit batch reaches the server as the manager's own T-SQL,
statement by known statement — one round trip — and DuckLake's conflict check and the probes a
read makes are single T-SQL statements ([Writing](./writing.md), [Performance](./performance.md)).

**Numbers.** On a 1000-table catalog against the PostgreSQL backend: 1.54x of its total time,
file-backed commits at parity (11.4 vs 9.0 ms), compaction six times faster, the first write into a
table 2.9x, commits of many files 5x, the attach 0.8 s.

**Not in this release.** The [limitations](./reference/limitations.md) page, in full: the first
inlined write's premium, bulk loads for many-file commits (the DuckDB 2.0 line), Fabric Warehouse
and Synapse as catalog stores, WebAssembly, the mssql extension's v0.2.5 rule for `mssql_scan`
inside a transaction.

**Platforms.** Linux amd64 and arm64, macOS arm64, Windows amd64 (MSVC and MinGW) — the
platforms the mssql extension ships for.
