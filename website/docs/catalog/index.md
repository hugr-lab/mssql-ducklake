---
title: Overview
sidebar_position: 1
---

# The catalog in SQL Server

A DuckLake catalog is a set of ordinary tables — `ducklake_snapshot`, `ducklake_table`,
`ducklake_data_file`, `ducklake_table_stats`, twenty-odd in all — plus one
`ducklake_inlined_data_<table>_<schema version>` table for every table that has had rows inlined.
With this extension they live in a SQL Server database, in `dbo` unless
[`METADATA_SCHEMA`](./attach.md) says otherwise. The data files never touch the server.

How the manager reaches them:

- The `ATTACH 'ducklake:mssql:…'` attaches the catalog database a second time, hidden, through the
  mssql extension (`__ducklake_metadata_<name>`), and DuckLake's own catalog queries run against that
  attached database as DuckDB SQL — the mssql extension pushes filters down and streams results
  back.
- What DuckDB cannot carry to SQL Server unchanged — the catalog's DDL, the inlined-data tables'
  DDL, and a few queries rewritten as one T-SQL statement for consistency or cost — the manager
  sends through `mssql_exec()` and `mssql_scan()`.
- Every transaction runs on one pinned server connection, so a commit's reads and writes see one
  consistent state; concurrent writers are serialized by DuckLake's snapshot protocol with a retry
  loop ([Writing](../writing.md#concurrent-writers)).

The three pages here cover [attaching](./attach.md) (connection strings and options), what
[shaping](./shaping.md) does to the database and why, and what the server and the login
[need](./requirements.md).
