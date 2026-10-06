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

A key this extension has since corrected is rebuilt rather than left alone: on attach the shaping
compares each key's columns with what the server has and drops the constraint when they differ, so
a catalog created by an earlier build ends up with the current key instead of the one it was made
with.

### Strings and their collation

DuckLake's catalog stores its strings — names, paths, and the per-file minimum and maximum values
it prunes with — as `VARCHAR` under **`Latin1_General_100_BIN2_UTF8`**, never `NVARCHAR`.
Two reasons: UTF-8 is what DuckDB writes, so nothing is transcoded; and BIN2 is DuckDB's byte order,
so a filter the server answers on a `min_value`/`max_value` column compares the way DuckDB computed
the statistics. A linguistic collation would prune wrongly and drop rows from a result. The
collation is explicit on every column because a database's own is usually a legacy `CI_AS` one.
This is why the server needs the UTF-8 collations of SQL Server 2019 ([Requirements](./requirements.md)).

### String lengths

DuckLake declares its strings without a length, which in T-SQL means one character, so the
catalog's are given one by what they hold. A `VARCHAR(MAX)` column travels as a large value, which
the mssql extension reads value by value rather than in batches: reading `ducklake_column` whole —
every catalog load reads it — costs 15–16 ms over 12,300 rows as `MAX` and 6–8 ms bounded.

| `META_LIMITS` key | default | columns |
| --- | ---: | --- |
| `name_length` | 256 | schema, table, column, view, macro and parameter names |
| `path_length` | 1024 | every `path` |
| `column_type_length` | 1024 | column and parameter types (`STRUCT(...)` included) |
| `default_length` | 1024 | default values and sort expressions |
| `text_length` | 2048 | view and macro SQL, tag values, commit messages, extra statistics |
| `stats_length` | 1024 | the min/max values in file and table statistics |

The lengths are bytes of UTF-8, 1–8000, or `'max'`. Formats, dialects and other keywords take 64;
`ducklake_snapshot_changes.changes_made` stays `MAX`, since it grows with the objects of one commit.

- **A new catalog** takes `META_LIMITS`, over the defaults, at the `ATTACH` that creates it.
- **A catalog migrated from format 1.0** (`AUTOMATIC_MIGRATION TRUE`) is narrowed to what
  `META_LIMITS` names, and only that, after checking what it holds: a stored value longer than its
  new bound fails the attach before anything changes, naming the column, the value's length and the
  bound.
- **Any other catalog** keeps what it has — one created before this, `MAX` throughout. A
  `META_LIMITS` given to it is ignored with a warning.

The chosen lengths are recorded on the catalog (an extended property, `mssql_ducklake_limits`) and
shown, with every column's declared type, by `mssql_ducklake_catalog_info('lake')`
([Settings](../reference/settings.md#functions)). A name or path longer than its bound fails the
commit that writes it — the server refuses it, it is never truncated. A **statistic** longer than its
bound is stored as NULL instead: DuckLake reads that as unknown and prunes nothing on it, so the
file is read and the answer stays right. A min or max past a kilobyte prunes almost nothing anyway.

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
work; the `ducklake_inlined_delete_<table>` tables that hold inlined deletions of file-backed rows
are keyed on `(file_id, row_id, begin_snapshot)` the same way, so a flush can clear them. The table is created in the commit that creates the lake table, outside the transaction, so
that the mssql extension's metadata cache can see it before the first `INSERT` needs it.

### Forced parameterization

The last steps are two statements outside the catalog's own tables, database options. The first:

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

### Asynchronous statistics

And one more, the same way:

```sql
ALTER DATABASE CURRENT SET AUTO_UPDATE_STATISTICS_ASYNC ON;
```

A catalog grows by thousands of rows with every few hundred commits, so SQL Server's statistics on
its tables go stale often — and by default the query that finds them stale recomputes them before
it runs. After 1000 commits into one table, the first read of it waited 1.2 s for six statistics of
`ducklake_file_column_stats` to be rebuilt, for a statement that then took 9 ms. With the option
that query runs on the statistics it has and the update happens beside it: the same first read took
90 ms. PostgreSQL behaves this way already, its `ANALYZE` being a background job.

The opt-out is `SET mssql_ducklake_async_statistics = false`; like forced parameterization it is
best-effort, applied when the catalog is shaped, and left alone afterwards.

### The shape stamp

The version of all of the above is recorded as an extended property (`mssql_ducklake_shape`) on the
catalog's `ducklake_metadata` table — per catalog, invisible to DuckLake's queries, and gone the
moment the catalog's tables are. Attaching reads it in one query; the DDL only runs when the stamp
is missing or older than this build's shape version.
