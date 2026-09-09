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

**The fix** is to ask the whole question as one statement the server answers by itself, through a
single `mssql_scan` — the pattern `GetLatestSnapshotQuery` already uses here:

```sql
SELECT * FROM mssql_scan({METADATA_CATALOG_NAME_LITERAL}, '… TOP 1 … UNION ALL … ORDER BY table_id')
```

One statement is evaluated against one consistent state, so no part of it can disagree with any other
part. That is a stronger guarantee than the first version of this fix, which only stopped the
snapshot branch from reading its table twice: this one keeps holding if DuckLake's query ever reads
any table twice again.

It is also a round trip instead of five. The DuckDB-SQL form sends a separate `SELECT` per table —
`ducklake_snapshot`, `ducklake_snapshot_changes`, `ducklake_table_stats`,
`ducklake_table_column_stats` — measured at **25 batches against 14** for the same conflict check
inside a transaction, and after the change the debug log shows **no** separate `ducklake_snapshot`
scan at all. And `TOP 1` now reaches the server, so the snapshot branch stops reading every row of a
table that grows by one per commit.

The T-SQL differences from DuckLake's version are all forced, none discretionary: `JOIN … ON` because
`USING` is not T-SQL; `TOP 1` inside a derived table because a `UNION ALL` branch may not carry its
own `ORDER BY`; `CAST(NULL AS …)` so the union resolves its column types instead of guessing from an
untyped NULL; and no `NULLS FIRST`, which SQL Server neither has nor needs, since ascending order puts
NULLs first — which is what leaves the snapshot row where DuckLake's parser expects it. The column
list, the column order and both branches are otherwise unchanged, because the parser depends on all
of them; the types were checked coming back through the scan (`contains_null` arrives as a boolean,
`record_count` as a bigint, not as text).

Measured after the change, same fourteen alternating rounds: **mssql 0 of 14, postgres 0 of 14.**

What it does **not** buy is throughput, and the measurement says so. Six writers, ten commits each,
three rounds per timing, the two builds alternated: 19.49 / 19.54 / 20.69s for the five-statement
form against 18.46 / 19.96 / 18.32s for the one-statement form. The ranges overlap — the new form is
slower in one of the three rounds — so there is no wall-clock signal here, and the arithmetic agrees:
eleven round trips at about a millisecond, on the fraction of commits that retry, is roughly half a
second inside nineteen. The justification is the round trips and the guarantee, not the clock.

Nor does it move `make bench` or `make bench-scale` at all, and it cannot: `CheckForConflicts` runs
only when `i > 0`, so a single-writer benchmark never executes this query once. That is also why the
bug survived so long — no single-threaded run reaches the code it lives in.

Measured cost, for the record: the whole conflict query takes 0.002s against a thousand snapshots,
0.003s against ten thousand and 0.016s against a hundred thousand, and the `MAX` subquery and the
`TOP 1` forms are level on that axis. So `TOP 1` is not where the win is — the round trips are. An
earlier draft of this spec rejected the `mssql_scan` form by counting only the `TOP 1` saving, called
it "unavailable", and contradicted its own D2, which recorded the form as verified to work. It is
available, it does work, and it is what shipped.

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
- **Patching only the snapshot branch** — `FROM (SELECT * FROM ducklake_snapshot ORDER BY snapshot_id
  DESC LIMIT 1)` in DuckLake's own SQL. This was the first version, and it is correct: it took the
  concurrent-writer failures from 6 of 14 rounds to 0 of 14. It was replaced because it fixes the one
  branch that happens to read a table twice rather than the reason two reads can disagree, and
  because it leaves the query as five statements where one will do.
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
