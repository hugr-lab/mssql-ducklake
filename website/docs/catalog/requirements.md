---
title: Requirements
sidebar_position: 4
---

# What the server and the login need

### Server

| platform | catalog support |
| --- | --- |
| SQL Server 2019 and later | yes — the integration suite runs against SQL Server 2025 |
| Azure SQL Database, Azure SQL Managed Instance | expected: the UTF-8 collations and every statement the shaping runs, including `ALTER DATABASE CURRENT`, are documented for both; not yet exercised by the suite |
| SQL database in Microsoft Fabric | expected, for the same reasons; not yet exercised |
| Microsoft Fabric Warehouse, Azure Synapse dedicated SQL pools | **no** — the shaping needs enforced primary keys, `ALTER TABLE … ALTER COLUMN` and extended properties, which these do not offer. They remain fine *data sources* through the mssql extension; they cannot hold a DuckLake catalog |

The one hard requirement is a UTF-8 collation — `Latin1_General_100_BIN2_UTF8`, available since
SQL Server 2019 — and the attach checks for it first, with a clear error when it is missing
([why](./shaping.md#strings-and-their-collation)).

### The login

| permission | for | required |
| --- | --- | --- |
| `CREATE TABLE`, `ALTER` on the schema (`db_ddladmin`, or ownership of the schema) | the catalog tables, the keys, indexes and extended properties, the inlined-data tables | yes |
| `INSERT`, `UPDATE`, `DELETE`, `SELECT` on the schema (`db_datareader` + `db_datawriter`) | every commit and read | yes |
| `ALTER` on the database | [forced parameterization](./shaping.md#forced-parameterization) | no — best-effort; a warning is logged when it is refused |
| `VIEW ANY DATABASE` and the like | nothing — the manager never reads `sys.databases` beyond the row every login sees for its own database | no |

The login's default schema does not matter: the mssql extension answers `dbo` as the catalog's
default schema regardless, and a login that must write elsewhere names its schema with
[`METADATA_SCHEMA`](./attach.md#options).

### Client

- DuckDB **v1.5.5**; the extension is built for that release line and ships through the DuckDB
  community extensions repository for it.
- The mssql extension **v0.2.5 or newer** ([Versions](../versions.md)).
- Not WebAssembly: the mssql extension is raw TDS sockets, so neither extension has a wasm build.
