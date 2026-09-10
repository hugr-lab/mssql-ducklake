---
title: Shaping
sidebar_position: 3
---

# What the manager does to the database

DuckLake's own DDL creates the catalog; the manager then *shapes* it for SQL Server. Everything
below is idempotent and stamped with a shape version, so a catalog created by an older build is
brought up to date on its next attach, and a catalog whose tables were dropped and recreated is
shaped again.

### Primary keys

The mssql extension builds a row identity out of a table's primary key, and without one it refuses
`UPDATE` and `DELETE`. DuckLake's commit batch updates `ducklake_table_stats` on every write past a
table's first, and expiry, cleanup and compaction delete from a dozen tables — so every table
DuckLake updates or deletes from gets a primary key (`ducklake_table_stats(table_id)`,
`ducklake_data_file`'s statistics on `(data_file_id, column_id)`, and so on). Key columns are made
`NOT NULL`; the tables keyed on a name (`ducklake_metadata`, the tag tables) get a `VARCHAR(200)`
key column, inside SQL Server's 900-byte index limit.

### Strings and their collation

DuckLake's catalog stores its strings — names, paths, and the per-file minimum and maximum values
it prunes with — as `VARCHAR(MAX)` under **`Latin1_General_100_BIN2_UTF8`**, never `NVARCHAR`.
Two reasons: UTF-8 is what DuckDB writes, so nothing is transcoded; and BIN2 is DuckDB's byte order,
so a filter the server answers on a `min_value`/`max_value` column compares the way DuckDB computed
the statistics. A linguistic collation would prune wrongly and drop rows from a result. The
collation is explicit on every column because a database's own is usually a legacy `CI_AS` one.
This is why the server needs the UTF-8 collations of SQL Server 2019 ([Requirements](./requirements.md)).

### Indexes

Neither the PostgreSQL nor the SQLite manager adds indexes; on SQL Server the difference measured
15x on the reads that matter:

| index | serves |
| --- | --- |
| `(table_id, begin_snapshot, end_snapshot)` on `ducklake_data_file` and `ducklake_delete_file` | the visibility condition every read carries, at the current snapshot and at older ones alike |
| filtered `WHERE end_snapshot IS NULL` on `ducklake_table`, `ducklake_column`, `ducklake_view` | the catalog load, which asks for the current state only |
| `(table_id, column_id)` on `ducklake_file_column_stats` | the per-column statistics a filtered read prunes with — a row per file per column, the largest table in the catalog |
| `(table_id, partition_key_index, partition_value)` on `ducklake_file_partition_value` | partition pruning; `partition_value` is bounded to `VARCHAR(200)` so it can be a key at all |

### The inlined-data tables

Rows below the inlining limit go into a `ducklake_inlined_data_<table>_<schema version>` table the
manager creates with T-SQL types: `BIT`, `SMALLINT`, `INT`, `BIGINT`, `DECIMAL(p, s)`, `DATE`,
`TIME(6)`, `DATETIME2(…)`, `DATETIMEOFFSET(6)`, `UNIQUEIDENTIFIER`, `VARBINARY(MAX)` — and
`VARCHAR(MAX)` for what SQL Server cannot hold exactly ([Writing](../writing.md#inlining-and-types)).
Each has a primary key on `(row_id, begin_snapshot)`, so DuckLake's inlined `UPDATE` and `DELETE`
work. The table is created in the commit that creates the lake table, outside the transaction, so
that the mssql extension's metadata cache can see it before the first `INSERT` needs it.

### Forced parameterization

The last step is one statement outside the catalog's own tables:

```sql
ALTER DATABASE CURRENT SET PARAMETERIZATION FORCED;
```

Every query DuckLake and the mssql extension send carries its literals in the text — a table name,
a `table_id`, a snapshot id — and SQL Server caches plans by text, so without the option each
distinct value is a plan of its own, compiled on first use: about 35 ms per table the first time it
is touched. With it the server parameterizes the literals itself and one plan serves every value.
On the 1000-table benchmark this took a quarter off the total and made the first write into each
table two to three times faster ([Performance](../performance.md)).

It is database-wide, so it has an opt-out — `SET mssql_ducklake_forced_parameterization = false`
before the attach that shapes the catalog — and it is best-effort: a login allowed to create the
catalog's tables but not to alter the database gets a working catalog without it, and a warning in
`duckdb_logs()` naming the statement for someone who may run it. It is applied when the catalog is
shaped, not on every attach: set it back and it stays back.

### The shape stamp

The version of all of the above is recorded as an extended property (`mssql_ducklake_shape`) on the
catalog's `ducklake_metadata` table — per catalog, invisible to DuckLake's queries, and gone the
moment the catalog's tables are. Attaching reads it in one query; the DDL only runs when the stamp
is missing or older than this build's shape version.
