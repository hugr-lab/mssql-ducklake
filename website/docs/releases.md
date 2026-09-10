---
title: Releases
sidebar_position: 8
---

# Releases

Versions of this documentation follow the releases: the dropdown shows the docs of each tag, and
*Next* is what is being written for the coming one.

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
