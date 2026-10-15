# Spec 020: the catalog load in one T-SQL statement, and a mirror behind it

- **Status**: phase A implemented (branch `spec/020-catalog-mirror`); phase B open
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

`MSSQLMetadataManager::GetCatalogForSnapshot` overrides the base. It sends one parameterised T-SQL
statement (`@s` = the snapshot id) through `mssql_scan_params_unsafe` with the shape given
(`columns := {...}`), as the file list does (specs/008), so the bind sends nothing.

- **One result set.** `UNION ALL` of one branch per kind (schema, table+column, table tag, column
  tag, inlined table, view, view column tag, macro, macro impl, macro parameter, partition, sort). It
  is a fixed tagged row: `kind TINYINT`, five `BIGINT` ids, a handful of `VARCHAR` slots and `BIT`
  flags.
- **Assembled on the client.** The rows come back ordered by kind and then by the base's order. The
  client builds the structures the base builds; the base's helpers are reused where they are
  reachable (`FromRelativePath`, the type parsing).
- **One read.** One statement replaces seven queries and their sub-scans. Under
  `READ_COMMITTED_SNAPSHOT` it is one statement-consistent read.
- **The joins stay on the server and use what exists.** `pk_ducklake_column (table_id, column_id,
  begin_snapshot)`, `ix_ducklake_column_live` and the tag keys already lead with the ids the joins
  use. The plan is checked with `SET STATISTICS IO` on the 1000-table catalog before an index is
  added.
- **Fallback.** A statement that fails is an error naming it, never a silent fallback. Under
  `MSSQL_DUCKLAKE_STRICT_BATCH` it has the same strictness as the commit batch.
- **No state, so no staleness.** Every call is a fresh read of committed rows. Other nodes cannot
  make it wrong.

### Phase B: the mirror (only if phase A leaves the rebuilds dominant)

Phase A cuts the cost of a rebuild, not the number of rebuilds, and every rebuild still reads every
live table and column. Phase B removes the re-reading.

#### B1: what is kept, and where

- **Contents.** All rows of the versioned tables with `begin_snapshot <= as_of` that were still
  visible at `base` or later, plus their children.
- **Bounds.** `base` is the snapshot of the first load. `as_of` is the last snapshot caught up to.
- **Where it lives.** In the DuckDB instance's object cache under
  `mssql_ducklake:catalog_mirror:<attached database oid>`, like the stats cache of spec 019 and the
  ready marker. So each ATTACH has its own mirror, and several lakes attached in one process never
  share one. It is guarded by its own mutex. Round trips run outside the lock, and the state is
  copied in and out, as in 019.
- **Answering a rebuild.** A rebuild at a snapshot `S` with `base <= S <= as_of` is answered by
  filtering the mirror in memory with the same visibility predicate.

#### B2: catching up, in one small query

If `S > as_of`, one statement reads, for the range `(as_of, S]`:

- `MAX(snapshot_id)` of `ducklake_snapshot`, read first. The new `as_of` is `min(S, that)`.
- The rows of each versioned table with `begin_snapshot` or `end_snapshot` in the range. A drop or
  an alter sets `end_snapshot` on an old row, and a create inserts a row with `begin_snapshot`.
- The children of parents that began in the range.
- A fingerprint of `ducklake_inlined_data_tables`: `COUNT_BIG(*)` and
  `CHECKSUM_AGG(CHECKSUM(table_id, schema_version, table_name))`. When it differs from the cached
  one, the table's rows are read in the same statement, never by a second round trip. These rows
  change without a snapshot, so a snapshot range cannot see the change.

That is one round trip, and nothing comes back when nothing changed. The range predicates need
`(begin_snapshot)` and `(end_snapshot)` indexes on the versioned tables, a shaping change (a
`SHAPE_VERSION` bump). Their write cost is measured with the commit bench before they are kept.
The alternative is to scope the delta by `ducklake_snapshot_changes`, which spec 019 already reads.
It is decided by measurement.

#### B3: why other nodes cannot make it wrong

- **Committed history does not change.** A row's `begin_snapshot` is set by the commit that inserts
  it, and its `end_snapshot` is set once by the commit that ends it. So the visible set at a
  committed snapshot `S` never changes after `S` committed. The only exception is
  `ducklake_inlined_data_tables`, which is fingerprinted on every catch-up.
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
- **A snapshot older than `base`.** This is time travel to before the first load. The mirror never
  loads history backwards.
- **`MSSQL_DUCKLAKE_NO_CATALOG_CACHE=1`**, the off switch.
- **A mirror over its budget**: a row count from the catalog's size, measured. Beyond it the mirror
  drops itself, and the next rebuild loads it again from the current snapshot.

#### B5: pruning

`ducklake_expire_snapshots` deletes history that no one can ask for. When a catch-up sees that the
oldest snapshot has moved past `base`, rows that ended before it are dropped, and `base` moves up.

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
  remained: 9 ms without it. DuckDB orders the rows on arrival (`ORDER BY k, o1, o2, o3, o4`
  around the scan); that measured the same as a client-side `std::stable_sort`, and is simpler.

The remaining server cost is the joins and the conversions; the plan is seeks on the existing keys
(`pk_ducklake_column` leads with `table_id`). What phase A does not change is the number of loads:
the flush still makes one per table, each reading the whole catalog. That is what phase B would
remove, and the flush's 147 s at 1000 tables says it is still worth doing.

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
