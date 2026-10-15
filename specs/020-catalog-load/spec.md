# Spec 020: the catalog load in one T-SQL statement, and a mirror behind it

- **Status**: implemented, phase A (#42) and phase B
- **Date**: 2026-10-15
- **Author**: vgsml, with Claude

## Summary

DuckLake rebuilds its in-memory catalog (schemas, tables with their columns, views, macros,
partitioning, sort keys, tags) from the metadata database. The rebuild runs whenever a schema
version is not in DuckLake's own cache. That means after every CREATE, ALTER or DROP, after the
first inlined insert into a table, and for every old schema version a flush or time travel asks
for. Today the rebuild is seven catalog-path queries: DuckDB joins them on the client, and every
sub-scan costs a round trip. Phase A makes the rebuild one T-SQL statement through
`mssql_scan_params_unsafe`, answered by the server alone. Phase B, gated on phase A's numbers, keeps
the catalog's versioned rows in memory per ATTACH and catches up with one small query. Then most
rebuilds read nothing they have read before. Both phases stay correct when other DuckDB processes
write the same catalog at the same time.

## Problem

Measured on the merged 2.0 pins (`main` f39c553), mssql, `metadata-log` plus `mssql_ducklake_trace`
plus `sys.dm_exec_query_stats`:

- **600 commits** (300 inlined first inserts, then 300 file-backed inserts into 300 tables of 41
  columns): 611 rebuilds, 13 s of the 17.6 s of metadata time. The tables and columns query alone
  took 8.7 s. Every CREATE TABLE and every first inlined insert causes one rebuild (300 + 300 + 11 observed). The first inlined
  insert creates the table's inlined data table and registers it in `ducklake_inlined_data_tables`.
- **`ducklake_flush_inlined_data` over 300 tables**: 16.7 s, of which 8.7 s are 299 rebuilds. Each
  table is flushed against the catalog at the schema version its inlined rows were written at.
  DuckLake's cache cannot hold those versions: a commit that changes the schema evicts the version
  before it (`invalidate_schema_cache(schema_version - 1)`). So all 300 versions are gone by the time
  the flush asks for them.
- **The cost grows with the catalog.** Every rebuild reads every live table and column, so the work
  per table grows with the number of tables. Flush per table: 42 ms at 40 tables, 56 ms at 300, and
  171 ms in the 1000-table scale bench (`flush_inlined` 171.4 s, `create_tables` 139.6 s).
- **Not the server.** Server elapsed time for all 600 commits' writes was about 0.5 s. The rebuilds
  are round trips plus client-side joins. Postgres pays the same rebuilds (211 s for its
  `flush_inlined`).

## Design

### What a rebuild reads (the contract)

`DuckLakeMetadataManager::GetCatalogForSnapshot(snapshot)` is a manager virtual. It returns
`DuckLakeCatalogInfo{schemas, tables, views, macros, partitions, sorts}`, built by
`BuildCatalogForSnapshot` from fifteen tables:

- **Versioned**, visible when `begin_snapshot <= S AND (S < end_snapshot OR end_snapshot IS NULL)`:
  `ducklake_schema`, `ducklake_table`, `ducklake_column`, `ducklake_view`, `ducklake_macro`,
  `ducklake_partition_info`, `ducklake_sort_info`, `ducklake_tag`, `ducklake_column_tag`,
  `ducklake_view_column_tag`.
- **Children** of a versioned parent, read through it: `ducklake_macro_impl`,
  `ducklake_macro_parameters`, `ducklake_partition_column`, `ducklake_sort_expression`.
- **Unversioned and mutable**: `ducklake_inlined_data_tables`. The base reads every row for a table,
  with no snapshot filter. Rows appear with an inlined insert and disappear when DuckLake drops a
  superseded empty inlined table after a commit, which happens without a new snapshot.

Both phases must return what the base would return at that moment, field for field and in the
base's order. That includes:
- tables ordered by `table_id`, then `parent_column NULLS FIRST`, then `column_order`;
- tag lists;
- relative paths resolved against the data path;
- macro parameters nested under their implementations.

### Phase A: one statement

`MSSQLMetadataManager::GetCatalogForSnapshot` overrides the base. The catalog's rows come from one
parameterised T-SQL statement through `mssql_scan_params_unsafe`, with the shape given
(`columns := {...}`) as for the file list (specs/008), so the bind sends nothing.

- **One result set, one row per catalog row.** It is a `UNION ALL` of one branch per table: the
  versioned tables (schema, table, column, tag, column tag, view, view column tag, macro, partition
  info, sort info) with their `begin_snapshot` and `end_snapshot`, the children (macro
  implementations and parameters, partition columns, sort expressions), and every inlined data
  table. Each row has the same tagged shape: the kind, five `BIGINT` ids, begin and end, one
  `UNIQUEIDENTIFIER`, six strings and two flags.
- **Joined, filtered and ordered on the client.** `AssembleCatalog` builds what
  `BuildCatalogForSnapshot` builds from its seven queries: the same fields, the base's order where
  the base orders, and the same refusals. One function serves every read, a snapshot's rows (phase
  A) and the mirror's (phase B), because the joins are not done on the server.
- **The NULL placeholders are `VARCHAR(1)`.** A UNION column takes the widest of its branches, and
  a `MAX` placeholder made every string column `MAX` (see Measured).
- **Fallback.** A statement that fails is an error naming it, never a silent fallback.
- **No state, so no staleness.** At a snapshot, every call is a fresh read of committed rows.
  `MSSQL_DUCKLAKE_NO_CATALOG_TSQL=1` goes back to DuckLake's seven queries.

### Phase B: the mirror

Phase A cuts the cost of a rebuild, not the number of rebuilds, and every rebuild still reads every
live table and column. Phase B keeps the rows instead.

#### B1: what is kept, and where

- **Contents.** Every row of the versioned tables that a snapshot still alive can see (`end_snapshot
  IS NULL OR end_snapshot > oldest`), with their children and every inlined data table.
- **Bounds.** `base` is the oldest snapshot at the first load. `as_of` is the last snapshot read up
  to.
- **Where it lives.** In the DuckDB instance's object cache under
  `mssql_ducklake:catalog_mirror:<attached database oid>`, like the stats cache of spec 019 and the
  ready marker. So each ATTACH has its own mirror, and several lakes attached in one process never
  share one. It is never evicted: losing it costs a full load, and keeping it costs a few MB.
- **Locking.** It is guarded by its own mutex. Round trips run without it, and the assembly at a
  snapshot runs under it.
- **Merging.** A row is keyed by its table's key columns and, if versioned, its begin snapshot. A
  row read again only ever brings an `end_snapshot` that was open before, so reading a row twice,
  or two readers' rows in either order, ends in the same state.

#### B2: the three reads

Each read is one statement. The mirror's reads start with a statement of their own that takes
`@latest = MAX(snapshot_id)`, `@oldest = MIN(snapshot_id)`, and the fingerprint of
`ducklake_inlined_data_tables` (`COUNT_BIG(*)`, `CHECKSUM_AGG(CHECKSUM(...))`), returned as a meta
row.

- **The first load** (`HISTORY`) reads every row a live snapshot can see.
- **A snapshot past `as_of`** (`CHANGES`) reads the versioned rows with `begin_snapshot > @a OR
  end_snapshot > @a`, the children of parents that began after `@a`, and the inlined data tables
  only if their fingerprint moved. Then `as_of = @latest`. No index was added for the range
  predicates. This read runs once per new schema version, after DDL, and the bench below shows it
  costs less than phase A's load did.
- **A snapshot the mirror holds** (`base <= S <= as_of`, which is what a flush or a compaction asks
  for, one per table) reads only the fingerprint, and the inlined data tables if it moved. They
  change without a snapshot: this manager creates and registers an inlined data table outside the
  commit's transaction (specs/004 D2), and DuckLake drops a superseded empty one after a commit. The
  verify switch caught the first case on the first run: a held snapshot answered from memory missed
  a table registered a moment before. That read does not move `as_of`, because it looked at no
  versioned table.

#### B3: why other nodes cannot make it wrong

- **Committed history does not change.** A row's `begin_snapshot` is set by the commit that inserts
  it, and its `end_snapshot` is set once by the commit that ends it. So the visible set at a
  committed snapshot `S` never changes after `S` committed. The only exception is
  `ducklake_inlined_data_tables`, which is fingerprinted on every load (B2).
- **Snapshot ids have no gaps.** A commit takes `MAX(snapshot_id) + 1` under the
  `ducklake_snapshot` primary key. A second committer that collides waits or retries, and never
  skips ahead. So once snapshot `S` is visible as committed, every snapshot up to `S` is committed.
  Reading the latest snapshot first and bounding the delta by it is safe. A commit landing between
  the two reads is outside the bound and is read next time. This is the same argument as 019's
  "last snapshot before the stats".
- **A lagging mirror costs a round trip, not a wrong answer.** A transaction's snapshot is read
  from the server when it starts, and conflicts are checked on the server at commit (specs/007).
  Neither uses the mirror. A node whose mirror is behind pays one catch-up query before it answers.
  It never answers from an older state, because it never answers for a snapshot past `as_of`.
- **No push and no invalidation.** Nothing crosses nodes. Each node reads the server.

#### B4: when the mirror is passed by

In these cases the rebuild uses phase A's statement, and the mirror is not updated:

- **A transaction that has written** (`wrote_in_transaction`). It reads on its own connection
  inside its open server transaction, so it would see its own uncommitted rows.
- **A snapshot older than `base`.** That is time travel to a snapshot `expire_snapshots` has
  removed, which DuckLake refuses before it asks.
- **`MSSQL_DUCKLAKE_NO_CATALOG_MIRROR=1`**, the off switch.
- **A mirror over its budget**: `MSSQL_DUCKLAKE_CATALOG_MIRROR_ROWS`, 1,000,000 rows by default. A
  1000-table, 41-column lake with a deep history holds about 60,000. Beyond the budget the mirror
  empties itself, and that attach loads each snapshot from the server.

#### B5: pruning

`ducklake_expire_snapshots` deletes history that no one can ask for. When a read sees that the
oldest snapshot has moved past `base`, the rows that ended at or before it are dropped, and `base`
moves up.

#### B6: the verify switch

`MSSQL_DUCKLAKE_VERIFY_CATALOG=1` (on in the suite) answers every rebuild both ways, from the mirror
or phase A and from the base, and fails on any difference, naming the snapshot and the first field
that differs. Missing a mutation is how this design can be wrong, and this is how it is caught.

## Measured (phase A)

The one statement, against DuckLake's seven queries (`MSSQL_DUCKLAKE_NO_CATALOG_TSQL=1`), the same
build.

**300 tables of 41 columns, 600 commits then a flush** (trace timelines, runs alternated):

| | old path | one statement |
| --- | ---: | ---: |
| one catalog load during the flush, median | 15.6-20.9 ms (the tables+columns query alone) | 9.5-9.8 ms (all of it) |
| flush of 300 tables | 17.6-20.3 s | 11.2-14.0 s |
| 600 commits | 10.3-13.2 s | 10.3-11.8 s (no catalog load on this path) |

**The 1000-table scale bench, mssql** (one run each, machine load 5-23):

| phase | old path | one statement | |
| --- | ---: | ---: | ---: |
| `flush_inlined` | 171.5 s | 147.5 s | -14% |
| `merge_adjacent` | 85.3 s | 73.1 s | -14% |
| `partitioned_merge_adjacent` | 82.5 s | 71.2 s | -14% |
| `first_commits` | 19.8 s | 18.1 s | -9% |
| `create_tables`, `second_commits`, `deep_history`, `evolution` | | | within ±3% |
| **total** | **650.3 s** | **598.9 s** | **-8%** |

Two things shaped the statement, both measured on the server (`sys.dm_exec_query_stats`, 300
tables, per load):

- **The NULL placeholders.** A UNION column takes the widest type among its branches, so a
  `VARCHAR(MAX)` placeholder made every string column MAX: 26 ms of server CPU per load. With
  `VARCHAR(1)` placeholders the catalog's own bounded columns decide the width: 14 ms.
- **No ORDER BY on the server.** A sort of every row, wide strings included, was most of what
  remained: 9 ms without it. Phase A first had DuckDB order the rows around the scan, which
  measured the same as a client-side sort. Phase B's assembly orders them itself, because the
  mirror's rows are in no order at all.

The remaining server cost is the joins and the conversions; the plan is seeks on the existing keys
(`pk_ducklake_column` leads with `table_id`). What phase A does not change is the number of loads:
the flush still makes one per table, each reading the whole catalog. That is what phase B would
remove, and the flush's 147 s at 1000 tables says it is still worth doing.

## Measured (phase B)

**The 1000-table scale bench, mssql**, the mirror against phase A (`MSSQL_DUCKLAKE_NO_CATALOG_MIRROR=1`),
one run each, back to back (machine load 6-10):

| phase | DuckLake's 7 queries (earlier, same day) | phase A | mirror | mirror vs A |
| --- | ---: | ---: | ---: | ---: |
| `flush_inlined` | 171.5 s | 115.4 s | 78.6 s | -32% |
| `merge_adjacent` | 85.3 s | 57.6 s | 39.1 s | -32% |
| `partitioned_merge_adjacent` | 82.5 s | 53.8 s | 34.7 s | -35% |
| `first_commits` | 19.8 s | 27.8 s | 18.8 s | -32% |
| `create_tables` | 137.5 s | 124.0 s | 111.0 s | -10% |
| `evolution` | 35.0 s | 30.7 s | 25.8 s | -16% |
| `evolution_read_latest` | 11.3 s | 7.3 s | 2.8 s | -61% |
| `second_commits`, `deep_history`, the reattaches | | | | within ±5% |
| **total** | **650.3 s** | **529.9 s** | **419.6 s** | **-21%** |

Phase A measured 598.9 s that morning and 529.9 s here. The machine is shared, which is why each
comparison above is between back-to-back runs. Against DuckLake's own seven queries the catalog
load is now a third off the whole bench. Postgres, which pays the same rebuilds through the same
embedded DuckLake, took ~1100 s on this bench the same day.

**300 tables, the flush alone** (trace timelines, runs alternated): one catalog load is 1.1 ms with
the mirror against 6.3-6.5 ms for phase A, and the flush is 8.5 s against 10.2-10.5 s. The 1.1 ms is
the fingerprint read (B2). The rest of the flush is DuckLake's own reads of the inlined rows and its
parquet writes.

## Enforcement & security

- **Fail closed.** A catalog statement that fails is an error, with no silent fallback to a stale
  answer. The mirror only ever answers for snapshots it has caught up to.
- **No user input in the T-SQL.** The only value is the snapshot id parameter; the schema name is
  the manager's quoted identifier, as everywhere else.
- **A ducklake bump.** If DuckLake adds a field to `DuckLakeCatalogInfo` or a table to the rebuild,
  the verify switch fails the suite. The bump re-audit (CLAUDE.md, vendoring rule) gains this
  statement beside the commit batch's closed list.

## Testing

- **Suite.** The integration suite and `test-concurrent` run with the verify switch on.
- **New sqllogictest cases** covering time travel to before and after DDL, rename, drop and re-create
  under the same name, macros with several implementations, partition and sort changes, tags,
  nested types, and a flush of tables whose inlined rows span schema versions.
- **Several nodes.** A scenario for `scripts/bench/concurrent_writers.py`: node A loops over
  CREATE, ALTER, DROP and inlined inserts. Nodes B and C read, write and flush the same tables.
  Each node verifies every rebuild and commits everything.
- **Bench.** The 1000-table scale bench and the commit and flush workloads above, before and after
  each phase: `create_tables`, `first_commits`, `flush_inlined`, `evolution` and the reattaches.

## Alternatives considered

- **Keep more of DuckLake's own schema cache.** Its invalidation is DuckLake's, and the sources are
  never edited.
- **Invalidation messages between nodes.** There is no channel, and a missed message is a wrong
  answer. Reading the server is the channel.
- **A cache keyed by schema version.** Rejected because `ducklake_inlined_data_tables` changes
  without a version change. It is what DuckLake's own cache does, and why it invalidates broadly.
- **`FOR JSON` instead of a tagged `UNION ALL`.** It is one value to parse in the client, which
  would need a JSON parser on a hot path and loses the typed columns. Kept in reserve if the tagged
  rows become unwieldy.

## Follow-ups

- **Upstream.** A flush that groups tables by schema version would make one rebuild per version
  instead of one per table. That is upstream work, to be raised after the measurements and not
  filed without asking.
- **`scripts/bench/metadata_log.py`.** On the 2.0 line it must disable extension autoload, as
  `scale_catalog.py` does, or the session dies at the first flush. That fix lands with this spec.
- **`metadata-log` overstates concurrent flushes.** With ducklake#1680 a flush runs independent
  tables concurrently. Their queries queue on one connection, and each logged `elapsed_ms` includes
  the wait. Aggregates of one shape can exceed wall time; the trace's timeline is the measure.
