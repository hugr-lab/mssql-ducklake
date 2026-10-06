# Spec 018: bounded string columns, chosen at ATTACH

- **Status**: implemented
- **Date**: 2026-10-06
- **Author**: vgsml, with Claude

## Summary

The catalog's string columns are bounded `VARCHAR(n)` instead of `VARCHAR(MAX)`, with the bounds
chosen when the catalog is created - or when it is migrated from format 1.0 - through its
metadata parameters at `ATTACH`, recorded in the catalog, and shown by a function. A statistic
longer than its bound is stored as NULL (unknown), which DuckLake already treats as "do not prune".

## Problem

DuckLake declares its strings `VARCHAR` without a length, which in T-SQL is `VARCHAR(1)`, so the
shaping (specs/006) made every one of them `VARCHAR(MAX)`. A `MAX` column travels as PLP and takes
the mssql extension's unvectorised decode: measured over 12,300 rows x 6 columns, 16–18 ms against
7–8 for the same data in `VARCHAR(400)`, on catalog scans and raw scans alike, ~0.12 µs a value.
The catalog load reads `ducklake_column` whole on every load, so a large catalog pays it per load;
a file list pays it per file. Casting in the query does not help: the server's `CAST` costs more
than the decode it saves (a 1000-file list 6.7–7.0 ms as it is, 8.0–9.0 ms cast). The declared
length has to be the column's.

What the catalogs actually hold (21 catalogs in the integration database): names ≤ 25 bytes, paths
≤ 341, column types ≤ 14, defaults ≤ 4; statistics up to 36,000 bytes (inlined strings and blobs,
which DuckLake does not truncate); `changes_made` up to 5,591 (a commit over 1000 tables).

## Design

### The options

One `ATTACH` option of this extension's own, `META_LIMITS`, a struct of lengths:

```sql
ATTACH 'ducklake:mssql:…' AS lake (
    METADATA_SCHEMA 'x',
    META_LIMITS {'name_length': 256, 'path_length': 1024, 'column_type_length': 1024,
                 'default_length': 1024, 'text_length': 2048, 'stats_length': 'max'}
);
```

DuckLake turns any `META_*` option into a metadata parameter and forwards it to the metadata
database's `ATTACH` - the mssql extension's - where a STRUCT value breaks the statement (measured).
And DuckLake keeps those parameters in its catalog's private options, without an accessor. So the
option is taken where it is still visible: DuckLake is compiled into this extension, and its `ATTACH`
handler is wrapped at load (`StorageExtension::Find(config, "ducklake")`, the `attach` pointer). The
wrapper takes `META_LIMITS` out of the options, keeps it for the attached database, and calls
DuckLake with the rest. No DuckLake or mssql source is touched. An unknown key or a length outside
1–8000 / `'max'` fails the `ATTACH`, naming it.

| option | default | columns |
| --- | ---: | --- |
| `name_length` | 256 | schema, table, column, view, macro, parameter names; name-mapping source names |
| `path_length` | 1024 | every `path` |
| `column_type_length` | 1024 | `ducklake_column.column_type`, `ducklake_macro_parameters.parameter_type`, `ducklake_column_mapping.type` |
| `default_length` | 1024 | `initial_default`, `default_value`, sort expressions |
| `text_length` | 2048 | view and macro SQL, column aliases, tag values, `ducklake_metadata.value`, author, commit message and extra info, `extra_stats` |
| `stats_length` | 1024 | min/max in file, table and variant statistics |
| — (fixed) | 64 / 128 / 256 | formats, dialects, value types, sort direction and null order, transforms, shredded types, macro type, metadata scope / inlined table names / encryption keys |
| — (fixed) | `MAX` | `ducklake_snapshot_changes.changes_made` (grows with the objects of one commit; read by the conflict check, never by a load) |

0 means `MAX`. A value is 1–8000 (bytes of UTF-8). A column this list does not know - one a DuckLake
bump adds - is `MAX`.

### Which catalogs

- **A new catalog** (no shape stamp, no snapshot past 0): the options, or the defaults above.
- **A catalog migrated from 1.0 to 1.1**: the options given at that ATTACH, and only those -
  existing `MAX` columns are narrowed after checking that no stored value is longer; one that is
  fails the attach before anything is altered, naming the column, the longest value and the bound.
- **Any other existing catalog**: unchanged - `MAX` stays `MAX`, and options given are ignored with
  a warning.

The bounds are recorded as an extended property (`mssql_ducklake_lengths`) on `ducklake_metadata`,
next to the shape stamp and the migration marker, and read with them in one statement.

### Statistics past their bound

With `stats_length` bounded, a min or max longer than the bound is written as NULL, its exactness
too. DuckLake reads a NULL bound as unknown (`has_min`/`has_max` false; `bounds_unknown` when both
are, `ducklake_stats.cpp:94-106`), merges never resurrect it (`MergeStats`), and the file-list
filter keeps a file whose bound is NULL (`… IS NULL OR …`, `ducklake_metadata_manager.cpp:1748`).
The commit batch nulls them in the statistics `INSERT`s and the coalesced refresh. The appender
writes file statistics past the batch, so a catalog with bounded statistics does not use it; nor the
server-side commit (specs/005).

### Seeing it

`mssql_ducklake_catalog_info('lake')`: the recorded bounds, the shape version, the migration marker,
the database's forced parameterization and asynchronous statistics, and every string column of the
catalog with its declared type as the server reports it.

## Measured (2026-10-06)

The same 300-table lake twice, bounded (the defaults) and `MAX` throughout:

| inside a transaction | bounded | `MAX` |
| --- | ---: | ---: |
| `ducklake_column` whole, 12,300 rows | 5.7–8.0 ms | 15.4–16.3 ms |
| `ducklake_schema` (one row) | 0.54–0.99 ms | 0.67–0.71 ms |
| a lake-wide merge (300 catalog loads) | 18.3–18.9 s | 17.8–18.3 s |

The transfer is 2–2.7x faster bounded; a small scan does not change. On the merge it does not show:
since the catalog load reads the visible columns only (specs/015), a load moves about half of
`ducklake_column`, which puts the expected saving near 1 s of 18 - inside this machine's noise
(other workloads were running). The gain grows with the catalog: it is per row of every load.

One catalog load, step by step (the DuckDB profiler's phases and operators; the remote scans the
remainder), the same 300-table lake at the version with 200 tables, against postgres:

| | postgres | mssql, bounded | mssql, `MAX` |
| --- | ---: | ---: | ---: |
| total | 19.7 ms | 26.6 | 30.6 |
| parse + bind + optimizer + local operators | 14.8 | 16.0 | 15.3 |
| remote scans (16) | 4.75 | 10.40 | 15.10 |
| of which tables+columns (5 scans, ~8k rows) | 2.92 | 4.86 | 9.69 |

Bounded strings take 4 ms off a load (13%), all of it in the tables+columns transfer - so the merge
above saves ~1 s, which that measurement could not see. What remains against postgres is a fixed
~0.4–0.5 ms per catalog scan, visible on the empty views and macros tables (three scans each: 1.35–
1.93 ms against ~0.57), and ~0.6 ms of bind: the mssql extension's per-scan cost (sent there with
these numbers, 2026-10-06).

### Found on the way: a leftover inlined table

The inlined-data table is created outside the transaction (so the mssql extension can see it before
the batch). A commit that fails after that - here, a table name past its bound - left it behind
unregistered, and the next table taking the same id and schema version took the same name, found
it, and wrote into another table's columns (*Referenced column "long_v" not found*). Before creating
it, the manager now asks - on the transaction's own connection, since the shaping's ALTERs on a new
catalog hold locks any other connection would wait on - whether a table of that name exists that
no row of `ducklake_inlined_data_tables` names; such a leftover is dropped and created afresh.

## Testing

`limits.test` (89 assertions): new catalog with defaults and with options (the declared types; a name past its bound fails the
commit; a statistic past its bound is NULL and the file is still read); an existing catalog
unchanged with options ignored; a 1.0 catalog migrated with options (narrowed) and with a stored value
past the bound (the attach fails, nothing altered); the info function. Measured: a catalog load and a
file list before and after.
