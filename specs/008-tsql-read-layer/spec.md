# Spec 008: the T-SQL read layer — one probe through `mssql_scan`, and why not five

- **Status**: implemented
- **Date**: 2026-09-10
- **Author**: VGSML

## Summary

Every read of a lake table asks the catalog a handful of small questions — is there an inlined
deletion table, how many rows does this inlined table hold, what is the net data-file row count,
what are the global stats, where does this schema version begin. Each is a single-table scalar
query on DuckDB's catalog path. On a thousand-table catalog one of them costs **964 ms**: the
existence probe, whose miss makes the mssql extension reload the whole schema's metadata. This spec
moves that one to `mssql_scan`, where a query is text and there is nothing to load — the pattern of
specs/007 without the text match, since it is an ordinary virtual.

All five were moved first, and measured three ways against each other. The probe alone gives the
whole read-side win; moving the other four cost 30 – 45% on every commit whose stats refresh runs
them, because their catalog scans are what warm the extension's metadata for exactly the tables the
commit then writes. They stay where they were, and the reason is recorded here so nobody moves them
again on the same intuition.

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

One override, `GetInlinedDeletionTableName`'s read path. Where the base runs
`SELECT NULL FROM <table> LIMIT 1` through the catalog and reads its error state as "absent", the
manager runs

```
FROM mssql_scan({METADATA_CATALOG_NAME_LITERAL}, 'SELECT CASE WHEN OBJECT_ID(''{METADATA_SCHEMA_ESCAPED}.<table>'') IS NULL THEN 0 ELSE 1 END AS present')
```

through `transaction.Query(snapshot, …)`, so the base substitutes the placeholders exactly as it
does today. One row either way, no error semantics, and — the point — no metadata load: through the
catalog path a miss on a table that does not exist sends the extension to reload the schema, which is
what 964 ms was. The schema placeholder has to be the identifier form: the T-SQL travels inside a
DuckDB string literal whose quotes are doubled *before* substitution, so a placeholder expanding to a
quoted literal would arrive with quotes the doubling never saw. This is the sole source of its
statement, which is the rule mssql v0.2.5 imposes on the pinned connection (design 002 §3.1).

The override needed care because of what the caches around it mean. The base holds a
per-transaction set of tables it created — **private**, and cleared by a non-virtual
`ClearInlinedTableCaches` on every commit retry, which is right: the `CREATE` rolls back with the
attempt. And the catalog-level cache records "exists" **permanently**. The base is safe because it
consults its private set *before* the probe, so a table this transaction created never reaches the
permanent cache. An override cannot see that set. So it keeps the base's create path exactly —
calls the base, which creates the table and records it in its own set — and only records, in a set
of its own, that this transaction created it. On the read path it consults the catalog-level cache
as the base does, then **always probes** on a miss (seeing this transaction's own uncommitted table
through the pinned connection), and writes "exists" to the permanent cache only for a table this
transaction did *not* create. A stale entry in the override's own set after a retry can only
suppress a cache write, never produce a wrong answer, because the probe is fresh. The base's own
TODO ("using the error state to check for existence here is fragile") is closed on the way.

### The four that went and came back

`GetNetInlinedRowCount`, `GetNetDataFileRowCount`, `GetGlobalTableStats` and
`GetBeginSnapshotForSchemaVersion` were moved to `mssql_scan` in the first version of this change
and verified correct — values equal to the postgres backend's on the same sequence, all suites
green. Then three builds were run against each other, alternated, first round discarded: the build
before this spec, all five overrides, and the probe alone.

Write, 150 tables, seconds (rounds 2 – 4):

| phase | before | all five | probe only |
| --- | --- | --- | --- |
| first inlined insert | 7.96 / 8.51 / 9.00 | 8.80 / 8.54 / 9.16 | 8.38 / 9.08 / 8.36 |
| second inlined insert | 2.51 / 2.86 / 2.57 | **3.78 / 3.33 / 3.62** | 2.73 / 2.87 / 2.71 |
| file-backed insert | 2.43 / 2.64 / 2.81 | 2.71 / 3.04 / 3.08 | 2.75 / 2.67 / 2.65 |

Read, one 1000-table catalog, a fresh process per arm, seconds:

| phase | before | all five | probe only |
| --- | --- | --- | --- |
| attach | 0.72 – 0.75 | 0.74 – 0.75 | 0.72 – 0.74 |
| first read of a table | **0.29 – 0.46** | **0.06** | **0.06 – 0.08** |
| next table, warm repeat | 0.01 | 0.00 | 0.01 |

All five cost 30 – 45% on the commits whose stats refresh runs the counts, and bought nothing on
reads beyond the probe. The mechanism is the extension's metadata cache: the four counters' catalog
scans touch `ducklake_table_stats`, `ducklake_table_column_stats`, `ducklake_data_file` and the
inlined table — the tables the commit batch then UPDATEs through DuckDB's DML path, which needs
their metadata loaded. Through `mssql_scan` nothing is loaded, and the batch pays for it. The first
version of this spec called the cold load "the cost" and the scan "free"; on the write path it was
the other way round. `MSSQL_DEBUG=1` showed it as batches: 24 for the first commit in a process
against 17, the difference being those loads.

The measuring tool ships with the change. `scripts/bench/metadata_log.py` captures DuckLake's own
`DuckLakeMetadata` log over a workload — `SET logging_storage = 'memory'`, `enabled_log_types =
'DuckLakeMetadata'`, then `duckdb_logs()` — and aggregates it by query shape with count, total time
and path (`catalog` / `mssql_scan` / `execute`). It is how this spec's numbers were taken.

### The incident on the way, and the fix that rode along

Measuring this spec's write-path cost required alternating two builds over a fresh catalog, and the
first attempt hung: a commit retried for ten minutes on

```
MSSQL: UPDATE/DELETE requires a table with a primary key. Table 'dbo.ducklake_table_stats' has no primary key.
```

— retried, because DuckLake's `RetryOnError` matches the words `primary key`. The catalog had 178
tables, **no keys, no indexes, and a shape stamp saying "current"**. The chain: the previous run was
killed while its pinned connection held locks; the next reset recreated the tables, our shaping ran
into those locks and failed partway (its error swallowed by the script), the stamp — written last —
was never written, but the *previous* catalog's stamp survived on the **schema**, and every attach
after that trusted it.

Before specs/006 the marker was a constraint on the tables and died with them; putting the stamp on
the schema (006 D4) lost that. It now lives on `ducklake_metadata`, the anchor table DuckLake itself
probes for the catalog's existence, so a recreated catalog is shaped again, and the shaping drops a
schema-level leftover when it finds one. The integration test plants the stale schema stamp before
its own reset and asserts the attach neither trusts nor leaves it. No server deadlock was involved —
the client sat in the retry loop's `sleep_for`, and `sys.dm_exec_requests` showed nothing waiting.

## Enforcement & security

The query text is a constant in our source with the same placeholders the base substitutes;
no part of them is built from user input. Nothing about the trust boundary moves.

## Testing

- **Which path ran is asserted, not assumed.** The integration test enables the `DuckLakeMetadata`
  log and asserts that the base's probe shape does not appear after a read, that the `OBJECT_ID`
  scan does, and — deliberately — that the counters' catalog shapes still do. specs/006's appender
  test could not tell its paths apart; this one can.
- **The answers are the same.** Row counts and inlined-deletion visibility are asserted against
  known values on tables carrying data files, an inlined table, delete files and an inlined delete
  — the four sources `GetNetDataFileRowCount` subtracts. The number that count feeds is the scan's
  cardinality estimate (`ducklake_scan.cpp`), not anything `ducklake_table_info` shows; and
  `ducklake_table_stats.record_count` counts written rows without subtracting deletes — 102 for 100
  file rows plus 2 inlined, after one delete of each kind. That expectation was checked against the
  postgres backend on the same sequence before it was written down here, because the first draft of
  this test expected 100 and would have blamed the override.
- The existing suites, unchanged: 163 integration assertions on both commit paths, `make
  test-concurrent` (the conflict path runs `GetGlobalTableStats` through 007's query, not this one,
  but the retry path is where a type mismatch would surface), MERGE on inlined tables (specs/006 D3).
- **Measured**, `make bench-scale` at 1000 tables, this branch against the morning's `main` on the
  same server (not alternated — the builds are a rebuild apart — so the small deltas are noise):

  | phase | before | after | |
  | --- | ---: | ---: | ---: |
  | `filtered_read` | 2.93 | 1.61 | **−45%** |
  | `partitioned_read_latest` | 1.23 | 0.47 | **−62%** |
  | `partitioned_reattach` / `deep_reattach` | 3.18 / 3.44 | 2.09 / 2.29 | −34% / −33% |
  | `reattach`, `table_info`, `deep_read_*`, `partitioned_read_pruned` | | | level |
  | `evolution_read_latest` | 23.4 | 24.0 | level — the per-schema-version reload, specs/005 D12 |

  The write phases came out 10 – 12% slower in that pair. That was real, and it is the four
  counters — see "The four that went and came back"; with the probe alone the writes are level.

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
