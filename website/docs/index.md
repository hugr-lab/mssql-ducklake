---
title: Overview
sidebar_position: 1
slug: /
---

# DuckLake on SQL Server

`mssql_ducklake` is a [DuckDB](https://duckdb.org) extension that runs [DuckLake](https://ducklake.select)
with its **catalog in Microsoft SQL Server or Azure SQL**. It embeds the complete, unmodified DuckLake
source at a pinned release and adds a SQL Server metadata manager beside DuckLake's built-in
PostgreSQL and SQLite ones; the manager talks to the server through the
[mssql extension](https://hugr-lab.github.io/mssql-extension/) — native TDS, no ODBC.

```sql
INSTALL mssql FROM community;
INSTALL mssql_ducklake FROM community;
LOAD mssql_ducklake;

ATTACH 'ducklake:mssql:Server=host,1433;Database=lake_meta;User Id=…;Password=…' AS lake
    (DATA_PATH 's3://my-bucket/lake/');

CREATE TABLE lake.sales(id BIGINT, amount DECIMAL(18, 2), day DATE);
INSERT INTO lake.sales SELECT i, i * 1.5, DATE '2026-01-01' + i FROM range(100000) t(i);
SELECT count(*) FROM lake.sales AT (VERSION => 1);
```

Everything DuckLake does works as in the stock extension — snapshots, time travel, schema evolution,
data inlining, partitioning, the maintenance functions, `ducklake:postgres:` catalogs too — plus
SQL Server as a metadata catalog. The data files live wherever DuckLake puts them (local disk, S3,
Azure Blob, …); only the catalog is in SQL Server.

→ **[Getting Started](./getting-started.md)** · **[The catalog in SQL Server](./catalog/index.md)** · **[Performance](./performance.md)** · **[Limitations](./reference/limitations.md)**

## How it works

- **Embedded DuckLake.** The extension compiles DuckLake in, so its metadata-manager registry is
  reachable — a loaded stock `ducklake` extension keeps that registry behind hidden symbols, and no
  other image can add a manager to it. The price is that `mssql_ducklake` and stock `ducklake` are
  **mutually exclusive**: load one or the other, and load `mssql_ducklake` *before* the first
  `ATTACH 'ducklake:…'`, or DuckDB autoloads the stock extension for that prefix.
- **The manager only generates SQL.** Like the PostgreSQL manager, it never links its scanner: its
  T-SQL runs through `mssql_exec()` and `mssql_scan()`, resolved at runtime. The mssql extension
  ships unchanged.
- **The catalog is shaped for SQL Server.** Creating a catalog puts primary keys on every table
  DuckLake updates, stores strings as `VARCHAR` under a UTF-8 binary collation, adds the indexes
  DuckLake's reads want, and enables forced parameterization on the database — see
  [Shaping](./catalog/shaping.md).

## Status

**Experimental.** Released from the community extensions repository with
`INSTALL mssql_ducklake FROM community` ([Getting started](./getting-started.md)):

- v0.2.0 for DuckDB v2.0.0, with DuckLake catalog format 1.1;
- v0.1.2 for DuckDB v1.5.6 ([Releases](./releases.md)).

What is in place:

- the full DuckLake surface;
- DDL, inlined and file-backed writes, `UPDATE`, `DELETE`, `MERGE`;
- concurrent writers and re-attach;
- a server-backed integration suite and a concurrency test in CI.

A benchmark against the PostgreSQL backend on equal Azure VMs took 0.63× of its time, at 500
tables ([Performance](./performance.md)). The [limitations](./reference/limitations.md) page is the
honest list of what is not there yet.

The project's design and every measurement behind a decision are in the repository's
[specs](https://github.com/hugr-lab/mssql-ducklake/tree/main/specs); if the extension finds users,
the manager is meant to be contributed upstream to DuckLake, after which this extension becomes
unnecessary.
