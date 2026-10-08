# Spec 019: the global table stats, cached per catalog

- **Status**: implemented (branch `recon/global-stats-cache`)
- **Date**: 2026-10-08
- **Author**: vgsml, with Claude

## Summary

DuckLake main (174a6f56) reads the global stats of every table of a snapshot in one metadata query,
and a commit into one table is a new snapshot - so every commit read every table's stats. The
manager keeps those stats per attached catalog and brings them up to date from
`ducklake_snapshot_changes`: only the tables touched since the cached state are read again. The
same change made concurrent writers deadlock on SQL Server; the conflict check runs at a higher
deadlock priority and a deadlocked commit is retried.

## Problem

After the bump to ducklake main (2cbf5a21, on duckdb 4fbae437b22), the 1000-table scale bench went
from 492 to 801 s on SQL Server and from 901 to 1044 s on postgres. Traced per commit: the stats
read `SELECT table_id, column_id, record_count, ... FROM ducklake_table_stats LEFT JOIN
ducklake_table_column_stats` returned all 1000 x 41 rows, 14.9 ms (first commits) and 29.7 ms
(second commits) on SQL Server, 11.1 / 20.6 ms on postgres, plus as much again on the client
building DuckLake's cache entries for every table. It was ~1.5 ms when it read one table.

The same read takes S locks on every table's stats rows inside a writing transaction. With eight
concurrent writers, SQL Server picked one as a deadlock victim (error 1205) in 3 of 6 runs - in the
conflict check, which DuckLake runs before a retry attempt counts as retryable, so the commit
failed.

## Design

`MSSQLMetadataManager::GetGlobalTableStats(snapshot)` (a manager virtual) answers from an
`MSSQLGlobalStatsEntry`: every table's `DuckLakeGlobalStatsInfo` as of `as_of`, the last snapshot
whose changes it includes. The entry lives in the DuckDB instance's object cache under the
attached database's oid - one per ATTACH, never shared between lakes, gone with the instance (the
manager itself lives one transaction, so it cannot hold it). It is guarded by its own mutex.

On each call: the changes `WHERE snapshot_id > as_of`, by any node - the server is the truth, and
nothing is invalidated in-process. Parsed with DuckLake's own `SnapshotChangeInformation::
ParseChangesMade`:
- inserts, deletes, alters, compactions, merges, delete rewrites, inlined inserts/deletes and
  flushes: the table is read again (`GlobalTableStatsQuery(v1_1, table_id)`, its sizes filled);
- drops: the table leaves the cache;
- creations of tables, schemas, views and macros: nothing (a new table has no stats row until an
  insert, which is its own change).

Everything is read again, as the base does, when the cache is empty, when the ids are not
contiguous (changes expired), when a change does not parse, or when more than 64 tables moved; the
last snapshot is read before the stats, so a commit landing between is read again next time rather
than missed. A transaction that has written (`Execute`) may be reading its own uncommitted state:
it reads past the cache and does not update it. `MSSQL_DUCKLAKE_NO_STATS_CACHE=1` turns the cache
off.

The deadlocks: the conflict check (`MSSQLConflictCheckQuery`) starts with `SET DEADLOCK_PRIORITY
HIGH`, the commit batch with `SET DEADLOCK_PRIORITY NORMAL`, so a deadlock between them is the
batch's to lose - and `IsRetryableCommitError` makes its 1205 retryable.

## Measured

1000 tables, mssql, the bench to the end of second_commits:

| | no cache | cache | before the ducklake bump |
| --- | ---: | ---: | ---: |
| first_commits | 41.3 s | 16.8 s | 15.8 s |
| second_commits | 62.2 s | 20.1 s | 18.6 s |
| the stats read per commit | 16.7 / 30.2 ms | 1.2 / 1.2 ms | ~1.5 ms |

own_inlined with eight writers: 3/6 runs failed (1205) without the cache, 1/6 with it, 0/10 with
the priority and the retry.

## Enforcement & security

Fail-closed in the sense that matters: anything the cache cannot account for reads everything. The
cache is per attached catalog and per DuckDB instance; two lakes on one node, or two attaches of
one lake, never share an entry.

## Testing

`stats_cache.test`: two attaches of one lake (two oids - what two nodes are to each other) - one
writes files, inlined rows, deletes, an ALTER, a drop and re-create; the other then writes, and row
ids stay unique and counts agree. Then a second lake whose first table has the same table id: each
lake keeps its own counts and row ids. The integration suite (666 assertions) and
`make test-concurrent` (0 of 60 writers lost) pass.

## Alternatives considered

- **Returning only the requested table's stats.** `GetGlobalTableStats` is not told which table,
  and DuckLake caches a negative entry for every table missing from the answer - the other tables
  would lose their stats, and a commit merging into them would write wrong ones.
- **A different isolation level for the stats reads.** `READ_COMMITTED_SNAPSHOT ON` on the
  catalog's database removes reader locks altogether, the postgres semantics - but setting it needs
  exclusive access to the database, so it is a DBA step: measured and recommended in the docs, not
  set by the extension. `SNAPSHOT` transactions refuse the `ALTER TABLE` / `CREATE INDEX` the
  shaping and the migrations run inside a transaction. `NOLOCK` would let the conflict check see a
  snapshot that is then rolled back.
