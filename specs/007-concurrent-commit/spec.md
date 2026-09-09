# Spec 007: concurrent commits — one table, read twice

- **Status**: implemented
- **Date**: 2026-09-09
- **Author**: VGSML

## Summary

Four processes writing to one lake lost a writer in six of fourteen rounds, to

```
INTERNAL Error: Failed to commit DuckLake transaction.
Calling GetValueInternal on a value that is NULL
```

The cause is not a conflict, not a timeout and not DuckLake: the commit loop's conflict check reads
`ducklake_snapshot` **twice in one query**, and through an attached catalog those are two separate
SELECTs with no consistent read between them. A commit landing in the gap makes them disagree. The
fix is to read the table once.

## Problem

The failure was first attributed — by this author, in specs/006 — to a known upstream issue: "the
conflict check that depends on `UNION ALL` row order". That attribution was carried over from an
earlier session and never checked. It was wrong, and the control that showed it was wrong is the one
that should have been run first:

| backend | rounds losing a writer | errors |
| --- | ---: | ---: |
| mssql | **6 of 14** | 6 |
| postgres | **0 of 14** | 0 |

Same DuckLake, same commit loop, same workload, another remote catalog. A defect in DuckLake's own
conflict check would have taken postgres down too. It did not, so the defect was ours.

## Decisions

### D1 — read `ducklake_snapshot` once

`MSSQLMetadataManager::Query(DuckLakeSnapshot, string &)` now recognises exactly one of DuckLake's
queries and replaces it wholesale.

**What the query is for.** On a retry — `CheckForConflicts` runs only when the first commit attempt
failed — DuckLake asks one question that returns three things in one `UNION ALL`: the newest snapshot
row, the `changes_made` recorded since our snapshot, and the global table statistics. Its parser
takes the **first row** as the snapshot row and every later row as statistics.

**Where it breaks.** The snapshot branch is

```sql
FROM ducklake_snapshot WHERE snapshot_id = (SELECT MAX(snapshot_id) FROM ducklake_snapshot)
```

Read through an attached catalog, that is not one statement to the server. With `MSSQL_DEBUG=2` the
extension prints what it actually sends, and it is two separate scans of the same table:

```
generated query = SELECT [snapshot_id], [schema_version], [next_catalog_id], [next_file_id] FROM [dbo].[ducklake_snapshot]   -> 12 rows
generated query = SELECT [snapshot_id] FROM [dbo].[ducklake_snapshot]                                                        -> 13 rows
```

**Twelve and thirteen.** Each catalog scan is materialised on its own — the log says so in as many
words, `materializing (issue #239 — shared pinned connection)` — and nothing holds a consistent read
across them. Another writer committed between the two. So `MAX(snapshot_id)` came back as a snapshot
the outer scan had never seen, the predicate matched nothing, the snapshot branch was empty, the
first row of the result was a statistics row, and DuckLake read its NULL `snapshot_id` as an `idx_t`.

Postgres does not have this because its scanner reads inside one transaction snapshot.

**The fix** is one line of SQL. The replacement is DuckLake's query verbatim except for the snapshot
branch:

```sql
FROM ( SELECT * FROM {METADATA_CATALOG}.ducklake_snapshot ORDER BY snapshot_id DESC LIMIT 1 ) latest_snapshot
```

One reference to the table, therefore one scan — confirmed by the same debug output, 2 scans down to
1 — and one scan cannot contradict itself no matter what commits in parallel. `ORDER BY … DESC LIMIT
1` picks the same row as `= (SELECT MAX(…))` because `snapshot_id` is the primary key, so there are
no ties to break. Everything else is untouched, because DuckLake's parser depends on the column list,
the column order and both branches.

Measured after the change, same fourteen alternating rounds: **mssql 0 of 14, postgres 0 of 14.**

What it does not fix, and should be said plainly: neither form pushes the limit down. The scan that
goes to the server is `SELECT snapshot_id, schema_version, next_catalog_id, next_file_id FROM
ducklake_snapshot` with no `TOP` and no `ORDER BY` — DuckDB reads every snapshot row and takes the
top one locally. On a catalog with a deep history that is a full read of `ducklake_snapshot` on every
commit retry. It is half of what the original did, which read the table twice, so this is strictly
cheaper as well as correct; but the cheap form is the one `GetLatestSnapshotQuery` uses — `TOP 1`
inside an `mssql_scan` — and that is unavailable here, because on mssql v0.2.5 an `mssql_scan` has to
be the sole source of its query and this one also reads three other tables.

**Why interception, and how it fails safe.** `GetSnapshotAndStatsAndChangesQuery()` is `static`, so
there is no virtual to override — but the executor reaches it through `metadata_manager->Query(…)`,
which is virtual and ours. The match is against DuckLake's template *before* any placeholder is
substituted, which is why the `(DuckLakeSnapshot, string &)` overload is the one overridden: at that
point `{SNAPSHOT_ID}` is still a placeholder rather than a number that would defeat an exact
comparison. Nothing is parsed out of the query — the catalog and schema the replacement needs are the
manager's own and arrive through the same `{METADATA_CATALOG}` substitution the base applies next.

An exact match means a ducklake bump that edits the query stops matching, which would silently
restore the crash in a path only concurrent writers reach. So `ProbeServerCapabilities` compares
DuckLake's template against a verbatim copy once per attach and throws if they differ, naming what to
re-audit. A bump that touches this query fails the integration suite instead of shipping a
regression.

### D2 — the alternatives, and why not

- **`SNAPSHOT` isolation on the pinned connection** — the true analogue of what postgres gets, and it
  would fix this whole class rather than one query. It needs `ALLOW_SNAPSHOT_ISOLATION` on the
  database, which is an `ALTER DATABASE` that cannot run inside a transaction and needs rights this
  extension should not assume. Worth doing in the mssql extension; not a reason to leave the crash.
- **The whole query as one `mssql_scan`** — verified to work: rewritten into T-SQL (`JOIN … ON`
  instead of `USING`, and no `NULLS FIRST`, which SQL Server does not have and does not need since it
  sorts NULLs first ascending) it runs as a single server-side statement, atomically consistent, with
  the snapshot row first. Rejected as more machinery than the problem needs: it makes the whole query
  our SQL to maintain, and on v0.2.5 it is only safe while the scan is the sole source of its query.
  Reading one table once achieves the same guarantee with one changed line.
- **Fixing it in the mssql extension** — several materialised catalog scans in one plan on a pinned
  connection should share a consistent read. That is the real cure and it belongs there; this spec is
  what makes the lake correct in the meantime.

## Enforcement & security

Nothing about the trust boundary changes: the replacement is a constant in our source with the same
two placeholders the base substitutes, and no part of it is built from user input.

## Testing

`make test-concurrent` (`scripts/bench/concurrent_writers.py`) — six writers, ten file-backed commits
each, ten rounds, every writer into its own table so none of them genuinely conflict. Any writer that
does not finish with all of its rows fails the run. CI runs it beside the integration suites, because
a regression test nothing runs is barely better than none; it costs about seventy seconds.

It cannot be a sqllogictest: the failure needs two processes committing at once.

**The test was checked for sensitivity, and the first version failed that check.** At four writers it
passed with the rewrite disabled — a test that cannot fail. The switch
`MSSQL_DUCKLAKE_NO_CONFLICT_REWRITE=1` exists so that this stays demonstrable rather than assumed:

```
MSSQL_DUCKLAKE_NO_CONFLICT_REWRITE=1 make test-concurrent    # 3 writers lost over 60 runs
make test-concurrent                                          # 0 lost over 60 runs
```

The `--backends mssql,postgres` arm is what settled the attribution and stays available for the next
time something looks like a DuckLake bug.

## Follow-ups

- Raise the underlying inconsistency with the mssql extension: catalog scans materialised separately
  on one pinned connection inside a transaction see different server states, which any query reading
  a table twice can trip over. This spec fixes the one instance DuckLake happens to have.
- specs/006's Follow-ups list the upstream findings that remain, minus this one, which was never
  upstream.
