# Spec 008: the T-SQL read layer — five scalar lookups through `mssql_scan`

- **Status**: draft
- **Date**: 2026-09-10
- **Author**: VGSML

## Summary

Every read of a lake table asks the catalog a handful of small questions — is there an inlined
deletion table, how many rows does this inlined table hold, what is the net data-file row count,
what are the global stats, where does this schema version begin. Each is a single-table scalar
query, and each goes through DuckDB's catalog path, which costs the mssql extension a metadata load
of the table on first touch. On a thousand-table catalog one of those questions costs **964 ms**.
This spec moves the five of them to `mssql_scan`, where a query is text and there is nothing to
load. Same pattern as specs/007, without the text match: these are ordinary virtuals.

## Problem

Measured on the 1000-table catalog of `make bench-scale` (design 002 §1.1), the first read after
attach takes 3.4 s. Of that, one query:

```
964 ms   SELECT NULL FROM ducklake_inlined_delete_11 LIMIT 1
```

That is `GetInlinedDeletionTableName`'s read-path existence check — it runs the query and treats
`HasError()` as "does not exist". On a miss the mssql extension reloads the whole schema's metadata,
and on this catalog the schema holds thousands of objects. The same probe cost 41 ms on a two-table
catalog: it is the one read-path cost that grows with the size of the catalog.

Around it, per read:

| virtual | cold | warm | what it is |
| --- | ---: | ---: | --- |
| `GetNetInlinedRowCount` | 55 – 96 ms | 0 – 4 | `COUNT(*)` on one inlined table, per inlined table version |
| `GetNetDataFileRowCount` | 99 ms | 3 – 13 | `SUM − SUM − COUNT` over data files, delete files, inlined deletions |
| `GetGlobalTableStats` | 168 ms | 1 – 5 | one join, rows into `TransformGlobalStats` |
| `GetBeginSnapshotForSchemaVersion` | 4 ms | 1 | one row |

"Cold" is the metadata load the extension performs before a catalog scan of a table it has not seen;
"warm" is the query itself. Every lake table carries at least one inlined table, and after a flush
several versions of it (specs/005 D10: 1054 of 1155 empty), and each read counts all of them. The
benchmark shows the consequence: `filtered_read` 16x, `reattach` 6 – 13x, `deep_read_filtered` 10x
against postgres.

In the mixed-workload inventory (design 002 §1.2) reads through the catalog path took 3216 ms of a
5440 ms run; reads through `mssql_scan` took 9.

## Design

Five overrides in `MSSQLMetadataManager`, each replacing the base's query with

```
SELECT * FROM mssql_scan({METADATA_CATALOG_NAME_LITERAL}, '<T-SQL>')
```

and handing it to `transaction.Query(snapshot, …)` so the base substitutes `{METADATA_SCHEMA_ESCAPED}`
and `{SNAPSHOT_ID}` exactly as it does today. Each query is the sole source of its statement, which
is the rule mssql v0.2.5 imposes on the pinned connection (design 002 §3.1) and what
`GetLatestSnapshotQuery` already relies on.

| virtual | the T-SQL |
| --- | --- |
| `GetInlinedDeletionTableName` (read path) | `SELECT CASE WHEN OBJECT_ID('<schema>.<table>') IS NULL THEN 0 ELSE 1 END AS present` — always one row, no error semantics |
| `GetNetInlinedRowCount` | the base text as is: `COUNT(*) … WHERE {SNAPSHOT_ID} >= begin_snapshot AND ({SNAPSHOT_ID} < end_snapshot OR end_snapshot IS NULL)` |
| `GetNetDataFileRowCount` | the base text as is — `COALESCE`, correlated subqueries and the join are T-SQL |
| `GetGlobalTableStats` | `JOIN … ON cs.table_id = ts.table_id` instead of `USING`; column list and order unchanged, because `TransformGlobalStats` reads by position |
| `GetBeginSnapshotForSchemaVersion` | the base text as is |

`GetInlinedDeletionTableName` is one function holding two caches — the per-transaction set and
`catalog.CacheInlinedDeletionTableResult` — and the `create_if_not_exists` branch, which runs
`CREATE TABLE IF NOT EXISTS` through `transaction.Query` and calls `ClearCache()`. The manager
already overrides it, only to record the table name for `ClearCache`. The new override reproduces
the caches and the create branch and replaces the probe; the base's own TODO ("using the error
state to check for existence here is fragile") is closed on the way.

`GetNetDataFileRowCount` calls `GetInlinedDeletionTableName`, so the first row of the table above
fixes it twice.

Types coming back through `mssql_scan` were verified for this exact pair of tables in specs/007:
`contains_null` arrives as a boolean, `record_count` as a bigint.

**The measuring tool ships with the change.** `scripts/bench/metadata_log.py` captures DuckLake's
own `DuckLakeMetadata` log over a workload — `SET logging_storage = 'memory'`, `enabled_log_types =
'DuckLakeMetadata'`, then `duckdb_logs()` — and aggregates it by query shape with count, total time
and path (`catalog` / `mssql_scan` / `execute`). It is how this spec's numbers were taken and how
the next one's will be.

## Enforcement & security

The five query texts are constants in our source with the same placeholders the base substitutes;
no part of them is built from user input. Nothing about the trust boundary moves.

## Testing

- **Which path ran is asserted, not assumed.** The integration test enables the `DuckLakeMetadata`
  log and asserts that none of the five base shapes appear after a read —
  `count(*) FROM duckdb_logs() WHERE message LIKE '%FROM … ducklake_inlined_delete_% LIMIT 1%'`
  is 0, and likewise for the others. specs/006's appender test could not tell its paths apart; this
  one can.
- **The answers are the same.** Row counts, `ducklake_table_info` output and inlined-deletion
  visibility are asserted against known values on a table with data files, an inlined table, a
  delete file and an inlined delete — the four sources `GetNetDataFileRowCount` subtracts.
- The existing suites, unchanged: 163 integration assertions on both commit paths, `make
  test-concurrent` (the conflict path runs `GetGlobalTableStats` through 007's query, not this one,
  but the retry path is where a type mismatch would surface), MERGE on inlined tables (specs/006 D3).
- **Measured**: `make bench-scale` at 1000 tables, `filtered_read`, `reattach`, `deep_read_*`
  before and after, rounds alternated; and `metadata_log.py` over the mixed workload, where the
  catalog-path read time is expected to drop from 3216 ms by the sum of the cold costs above.

## Alternatives considered

- **Leave it, warm the cache instead.** The cold cost is the extension loading a table's metadata;
  priming every inlined table at attach would move the cost, not remove it, and on a thousand-table
  catalog with several inlined versions per table it is the attach that is already 6 – 13x.
- **Rewrite the file list too.** Its stats CTE would be an `mssql_scan` beside catalog scans of
  `ducklake_data_file` — the mixed shape v0.2.5 fails on (design 002 §5). After the 2.0 bump.
- **Intercept in `Query` by text, as 007 does.** Unnecessary here: these are virtuals, and an
  override survives a ducklake bump without a guard because the base's SQL is never matched.

## Follow-ups

- specs/009: the first inlined write, where the same cold load is paid on the write side.
- After the 2.0 bump: `GenerateFileColumnStatsCTEBody` as an `mssql_scan`, the catalog load by its
  price (design 002 §3.4, §5).
