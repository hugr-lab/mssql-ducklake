---
title: Troubleshooting
sidebar_position: 3
---

# Troubleshooting

**`mssql_ducklake embeds ducklake and cannot be loaded together with the ducklake extension`**
— stock `ducklake` is already loaded in this process, usually by autoloading: an
`ATTACH 'ducklake:…'` ran before `LOAD mssql_ducklake`. Start a fresh process and load
`mssql_ducklake` first.

**`mssql_ducklake needs the mssql extension`** — `INSTALL mssql FROM community; LOAD mssql;`
then `LOAD mssql_ducklake`. The load tries to autoload mssql itself and fails with this message when
it cannot.

**`Schema 'main' not found in MSSQL database`** on attach — the mssql extension is older than
v0.2.5, which answers `dbo` as the catalog's default schema. `SELECT mssql_version();` to check;
`FORCE INSTALL mssql FROM community;` to update. With v0.2.4 the attach works with
`METADATA_SCHEMA 'dbo'`.

**`This SQL Server has no Latin1_General_100_BIN2_UTF8 collation`** — the server is older than
SQL Server 2019. The catalog's strings need a UTF-8 collation ([why](../catalog/shaping.md#strings-and-their-collation)).

**`UPDATE/DELETE requires a table with a primary key`** on a write — the catalog is missing the
keys the shaping adds: a run died between DuckLake's DDL and the shaping, or the tables were
recreated behind the extension's back. Detach and attach again: the attach notices a missing or
stale shape stamp and re-applies the DDL, which is idempotent.

**`Failed to key and index the DuckLake catalog for SQL Server`** — the login may create tables
but not alter them (keys, indexes, extended properties need `ALTER` on the schema). See
[Requirements](../catalog/requirements.md#the-login).

**A warning about `PARAMETERIZATION FORCED` in `duckdb_logs()`** — the login may not alter the
database. Everything works; the first touch of each table costs a plan compile until someone with
the permission runs `ALTER DATABASE CURRENT SET PARAMETERIZATION FORCED;` on the catalog's database
([Shaping](../catalog/shaping.md#forced-parameterization)). Logging is off by default:
`SET enable_logging = true;` before the attach to see it.

**An `IO Error` from the mssql extension on a user query** that mixes `mssql_scan()` with a scan of
the attached catalog inside a transaction, or `TDS parse error: Unknown token type` after calling a
stored procedure through `mssql_scan()` — mssql v0.2.5 limitations, both fixed on its 2.0 line
([Limitations](./limitations.md)). Run the `mssql_scan()` alone, outside the transaction.

**Slow first read or first write into every table** — check `is_parameterization_forced` for the
catalog's database (`SELECT is_parameterization_forced FROM sys.databases WHERE database_id = DB_ID()`
through `mssql_scan`); if it is 0 the shaping could not set it. [Performance](../performance.md)
has the numbers.

**Seeing what the manager does** — `MSSQL_DEBUG=1` on the process prints every batch the mssql
extension sends, and the DuckLake metadata log (`enabled_log_types = 'DuckLakeMetadata'`) records
every catalog query with its time; `make metadata-log WORKLOAD=…` aggregates them.
