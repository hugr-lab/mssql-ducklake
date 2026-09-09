# Spec 005: phase 2 — the commit as one call to the server

- **Status**: draft
- **Date**: 2026-09-09
- **Author**: VGSML
- **Depends on**: [004](../004-manager-phase-1/spec.md) (the manager, and the measurement that motivates this)

## Summary

A commit stops being a conversation. Instead of DuckDB issuing every metadata statement of a commit
over its own round trip, the manager stages the commit's **data** on the server and calls one
stored procedure, `ducklake_commit`, which applies it and retries conflicts without asking the
client anything. DuckLake already has this shape — the quack manager does exactly it — and most of
the client half is machinery we can reuse rather than write.

## Problem

Phase 1 met its correctness goal and missed its performance one. Measured against the postgres
backend on the same workload in the same process (specs/004, "Measured"): **~20x slower overall**,
with attach at 40x, `create_table` at 37x and a small commit at 9x.

The cause is not a mistake to fix. Phase 1 deliberately sends DuckLake's own SQL through DuckDB —
that is what freed it from rewriting SQL it did not generate, after rewriting produced a silent
data-corruption bug on the way (specs/004 D1). The postgres manager avoids both problems only
because DuckLake's SQL *is* postgres's dialect: it can pass a whole commit through
`postgres_execute` untranslated. There is no equivalent move for T-SQL.

So the round trips have to go away without translating anything. That is possible if what crosses
the wire is the commit's **data** rather than its **SQL**.

## Design

### D1 — reuse DuckLake's staging, not its SQL

`DuckLakeStagedCommit::Build` already turns a transaction into: create seventeen staging tables,
truncate them, `INSERT` the commit's rows into them, then call `ducklake_commit(...)`. quack sends
that batch to a DuckDB server that has the ducklake extension loaded.

We keep the first part and replace the last. The batch's staging half is ordinary DuckDB SQL, so it
runs against **local temporary tables in the client's own DuckDB** — no server, no round trips.
What lands there is exactly the commit, already normalized by ducklake's own code, which is the
part we most want not to reimplement (and not to re-audit on every submodule bump).

### D2 — the data crosses by BCP, into a temp table inside the transaction

The seventeen local tables are copied to SQL Server with
`COPY <local> TO 'mssql://<catalog>/#<staging table>' (FORMAT 'bcp', CREATE_TABLE true)` — the
bulk-load path, on the transaction's own connection. Empty tables are skipped, which is most of them
for a typical commit.

The staging tables on the server are **`#temp` tables on the transaction's own connection**, not
permanent ones. That was not the first plan — permanent tables named per session were — and the
difference matters more than it looks:

- they are **transactional**: verified against the server, rows bulk-loaded into `#stage` inside the
  metadata transaction are visible to the next statement on that connection, and a `ROLLBACK` leaves
  nothing behind at all (`tempdb.sys.tables` empty afterwards). A failed commit therefore cleans up
  after itself, with no truncate-before-use discipline and no orphan to sweep;
- they are **private to the connection**, so two concurrent commits cannot collide by name and no
  `##` global temp is needed;
- and the bulk load still runs on the pinned connection, so nothing extra is opened.

One consequence to carry into D3: a `#temp` table takes **tempdb's** collation, not the database's
(mssql-extension spec 060 §9). Any comparison the procedure makes between a staged string column and
a catalog column has to name the collation explicitly, exactly as the catalog's own DDL does.

Checked before committing to this, because it is the assumption the design rests on: every one of
the seventeen staged tables is flat — `BIGINT`, `VARCHAR` and `BOOLEAN` columns only, no nested
types anywhere (`DuckLakeStagedTable::Columns`). DuckLake normalizes a commit into scalars before it
stages it, which is exactly what BCP can carry.

### D3 — the apply, in T-SQL — and why it is a batch rather than a procedure

A stored procedure is the natural home for it: versioned, compiled once, called by name. It is not
usable today. Calling any procedure through this extension desynchronizes its TDS parser by two
bytes, because `RETURNSTATUS` (0x79) is read as if it carried a length field when [MS-TDS] makes it
fixed at five bytes — filed as **hugr-lab/mssql-extension#323** with the wire evidence. A
desynchronized connection cannot be recovered mid-commit, so this is not a matter of retrying.

A plain batch produces no `RETURNSTATUS`, so the apply is sent as one. It costs compilation per
commit and gives up the version-in-the-catalog story, and it goes back to being a procedure when
#323 lands.

The apply is ours to write, and this is what it does. It does what the client loop does today, on the server:

1. allocate the next `snapshot_id` and `schema_version`, under the isolation the retry loop needs;
2. insert the staged data files, their column statistics and partition values; the delete files; the
   inlined rows, deletes and file-deletes; the dropped files; the name maps; the compactions;
3. update `ducklake_table_stats` and `ducklake_table_column_stats` from the staged rows;
4. write `ducklake_snapshot` and `ducklake_snapshot_changes`;
5. on a conflict, roll back and retry from step 1 up to the retry configuration, which arrives as
   procedure parameters — the point of the exercise is that the retry costs no round trip;
6. return `(snapshot_id, schema_version, had_flushes)`, the three values the client needs back.

This is the one place where a piece of the commit protocol is duplicated outside DuckLake, and the
duplication is the price of the round trips. A ducklake submodule bump therefore re-audits this
procedure alongside the transpiler-free statement inventory (CLAUDE.md's vendoring rule).

### D4 — the scope is a data-only commit, and the rest falls back

`CanSkipSnapshotFetch` returns true only for a commit that creates no schema, table, view or macro,
alters nothing, drops nothing, and needs no new inlined table — quack's `IsDataOnlyCommit`, which we
reuse verbatim. Everything else takes the phase-1 path, which works. That keeps the procedure to the
hot shape (insert, delete, compaction) rather than the whole of DDL.

`ProbeServerCapabilities` — which already runs once per attach and checks the catalog's shape —
additionally verifies that the procedure exists at the manager's version, and only then calls
`SetRetrialsServerSide(true)`. A catalog whose procedure is missing or stale simply stays on phase 1
until the next initialization refreshes it.

### D5 — what the staging costs, measured before the procedure exists

The staging half landed first and was benchmarked on its own, with the client loop still applying
the commit — so the numbers are pure overhead, and that is what makes them informative. Against the
same workload as specs/004: total 7.3s where phase 1 alone was 4.9s, with a small commit going 0.41
→ 0.88 and update/delete 0.20 → 0.91.

**Staging a small commit costs about what committing it costs.** The reason is round trips again:
a bulk load is one per non-empty staged table, and a small commit has four to six of them — the same
order as the statements the client loop sends. The saving only appears where one bulk load replaces
*many* rows: a commit with hundreds of data files, or inlined data past a few rows.

Two things follow, and neither is a surprise once stated:

- the fast path wants a **threshold**, the way the inlined-write path does (research note §7): below
  it, the client loop is already the cheaper answer;
- the procedure has to be worth more than the staging costs, which means it must replace the client
  loop rather than run beside it. Until it does, phase 2 is a slowdown — which is why it is armed
  but inert (D4), not because arming it is risky.

## Enforcement & security

The procedure is created by us and takes no SQL from the client: its parameters are the schema name,
a schema version, and retry numbers. The staged rows arrive as data through BCP, never as statement
text — which is the property that makes this safe where rewriting SQL was not.

Fail-open, not fail-closed: every reason to refuse the fast path (no procedure, wrong version, a
commit that is not data-only) falls back to phase 1 rather than failing the commit.

## Where this stands

Landed and exercised on every data-only commit: the staging (D1, D2), measured in D5.

Written but **off by default** behind `MSSQL_DUCKLAKE_SERVER_COMMIT=1`: the apply. It gets further
than that phrasing suggests — against the server the batch applies a data-file commit correctly and
hands back the right values (`snapshot=2 schema_version=1`), which is the hard half — but the full
cycle then fails with `INTERNAL Error: Calling GetValueInternal on a value that is NULL`, after our
own reads have succeeded, and the database is invalidated. So the fault is in what happens on the
DuckLake side of a server-side commit: `ApplyServerSideCommit`, or the transaction state we leave
behind. quack does two things there we do not — `ClearCache()` and, on flushes,
`DropEmptySupersededInlinedTablesClientSide()` — and that is the first place to look. It wants a
debugger and a fresh head, not another guess.

Until then `ProbeServerCapabilities` does not arm the fast path, so DuckLake never takes it and the
default build is exactly phase 1, which stays green.

Two lessons already paid for, both about SQL Server rather than about DuckLake:

- a `#temp` table created inside a stored procedure dies with it, so the result table is created by
  the caller in the same batch;
- and the RETURNSTATUS desync above, which is what turned the procedure into a batch.

## Testing

- The integration suite gains a data-only commit cycle asserted twice: once with the procedure in
  place and once with `SetRetrialsServerSide` disabled, with identical results — the fast path must
  be indistinguishable from the slow one.
- Conflict handling: two connections committing to the same table, one of which must retry
  server-side and succeed.
- `make bench` is the acceptance criterion. The target is the one specs/004 missed: not worse than
  the postgres backend on the same workload.

## Alternatives considered

- **Send the staging INSERTs to the server as SQL** rather than BCP: simpler, but it is the same
  round trip per statement that phase 1 already pays, so it would not move the number that motivates
  this spec.
- **Reimplement the staging emit ourselves** instead of running DuckLake's batch locally: more code,
  and it re-audits on every bump. The local temporary tables cost nothing.
- **Wait for an upstream ducklake manager contributed in-tree** (the traction track of spec 002):
  it would inherit the same problem — an in-tree SQL Server manager still has no `postgres_execute`
  equivalent.

## Follow-ups

- **Fill the local staging tables with an Appender rather than INSERT text** (spec 006, the
  optimization pass). DuckLake emits its staging as `INSERT` statements, which is SQL to parse for
  every row of a commit; an Appender writes the same rows straight into the local table. The catch is
  that the emit is ducklake's code, so using an Appender means reimplementing it here - the very
  thing D1 avoids to keep submodule bumps cheap. It is therefore a measurement question: how much of
  a commit's time is the local staging at all, once the round trips are gone. Nothing about the
  server side changes either way.
- The procedure's own migration story once it changes: recreate on version mismatch is enough while
  this extension is experimental, but a catalog shared by two client versions needs a rule.
- Compaction and `expire_snapshots` are data-only by the definition above but exercise the least
  tested parts of the procedure; they deserve their own scenarios before this is called done.
