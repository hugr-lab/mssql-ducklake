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

### D6 — the server assigns row ids, and next_row_id is carried forward

A staged data file carries a `row_id_start` only when it is a **flush of inlined data**, which keeps
the ids those rows already had. For every ordinary insert the column is NULL by design: the client
assigns row ids from the table's current `next_row_id` at commit time, so whoever applies the commit
has to do the same. The apply therefore computes them itself, per table, files in staging order (the
staged file id increases with `file_order` within a table, which reproduces the client's order), and
a file that only rewrites inlined rows into parquet (`partial_max` set) adds bytes but neither
records nor ids — `DuckLakeTableStats::MergeFileStats`, exactly.

`next_row_id` is **monotonic**: carried forward from the existing row and advanced by what was
inserted, never recomputed from the files present.

Passing the staged NULL straight through, as the first cut did, writes a NULL `next_row_id` into
`ducklake_table_stats`. Nothing complains at commit time — `GlobalTableStatsQuery` filters on
`record_count`/`file_size_bytes` being non-NULL but not on that column — and the failure surfaces
much later and nowhere near the cause: the next scan of the table reads the stats to estimate
cardinality and dies in `TransformGlobalStatsRow` with `Calling GetValueInternal on a value that is
NULL`, which invalidates the database. The lesson is the general one for this phase: **the apply
owes DuckLake every derived value the client would have computed**, and the catalog will not tell us
which ones we missed.

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

### D7 — measured against phase 1, which found two defects and a threshold

`make bench-paths` runs the same workload through both commit paths, differing only by the
environment variable that arms the apply. Phases that cannot depend on the commit path — attach,
reattach, a scan — come out within a few percent, which is the noise floor the rest is read against.
Round trips are counted separately, from the server's own `Batch Requests/sec` counter, as the
difference between a run of one commit and a run of twenty-one.

The first measurement said the fast path was **1.47x slower overall** and slower in every phase that
commits. Two causes, both real defects rather than costs of the design:

- **It cleared the mssql extension's catalog cache after every commit.** Copied from quack, which
  needs it because its apply creates inlined-data tables. Ours creates no table at all — it writes
  rows into catalog tables that already exist. The clear cost the whole schema's metadata: of the 66
  round trips an attach-plus-commit took, **39 were the extension re-reading table and column
  metadata**, not DuckLake asking anything. Now cleared only when the server reports a flush.
- **A commit the apply cannot take paid for both paths.** The shape was decided *after* staging, so
  such a commit did the full staging, bulk-loaded it to the server, threw it away, and then ran the
  entire client commit loop. It was the worst phase in the benchmark, 3.5x. The shape is knowable
  before any of that: `TransactionChangeInformation` names each one separately, so
  `IsDataFilesOnlyCommit` decides first and nothing is staged that will be abandoned.

Deciding before staging also made the staging check meaningful as an assertion, and it immediately
caught a third defect: a **partitioned** table's files carry partition values, which the apply did
not write. That had been hidden by the old after-the-fact fallback, which quietly sent every
partitioned commit to the client loop. The apply now writes `ducklake_file_partition_value`, reading
the staged table through `sp_executesql` so the statement is compiled only when that table was
loaded — an empty one is not bulk-loaded, and loading it would cost a round trip on every commit of
an unpartitioned table.

After the fixes, per commit: **16.0 round trips on phase 1, 15.2 on phase 2**, and by phase:

| phase | phase 1 | phase 2 | ratio |
| --- | --- | --- | --- |
| one commit, many data files (`wide_commit`) | 0.546 | 0.292 | **0.53x** |
| small file-backed commits (`file_commits`) | 0.610 | 1.045 | 1.71x |
| inlined commits (`small_commits`) | 0.815 | 0.700 | 0.86x |
| total | 6.125 | 6.007 | 0.98x |

**This is the threshold D5 predicted, now with a number on it.** The apply wins where it was designed
to — one commit carrying many data files, where a single bulk load replaces many rows of statement
text — and loses on a commit carrying one small file, where the bulk load is pure overhead. Overall
parity is not the goal; picking the path per commit is. The staging is local duckdb work until the
first bulk load, so the count is knowable for free before anything crosses the wire.

Skipping the snapshot fetch (`MSSQL_DUCKLAKE_SERVER_COMMIT_SKIP_FETCH=1`) is implemented and correct
— the catalogs match — but measures no better, so it stays off. Its invariant is worth writing down
because it is not obvious: DuckLake holds `snapshot_lock` across the flush call, and `GetSnapshot()`
takes that same non-recursive mutex, so a commit that skipped the fetch can **never** ask for the
snapshot afterwards. `CanSkipSnapshotFetch` must therefore answer for exactly the commits the apply
finishes without falling back — which is why both now ask `IsDataFilesOnlyCommit` and neither decides
anything after staging.

### D8 — warmed, and against postgres: the commit path is not where the time goes

`make bench` now runs each arm's whole workload once and throws the result away before timing, and
`make bench-paths` compares the two commit paths the same way. Warming matters because the first run
pays for the server's plan cache, its buffer pool and its file growth; alternating the arms per round
removed the rest of the ordering bias.

The warmup is now **in the measured process**, not only a discarded run before it. A discarded run
in another process warms the server; it cannot warm a connection pool, which does not outlive the
process that made it — and the pool is exactly what an mssql attach spends its time building. Each
arm therefore opens its backend, touches it, and **leaves it attached** before the first phase
marker; detaching the warm catalog throws away what was warmed. That alone took attach from 2.10s to
1.25s.

Warmed, against the postgres backend on the same workload, over five rounds:

| phase | mssql | postgres | ratio |
| --- | ---: | ---: | ---: |
| attach (creates the catalog) | 1.248 | 0.057 | 21.9x |
| reattach (opens an existing one) | 0.860 | 0.034 | 25.3x |
| small_commits | 0.646 | 0.110 | 5.9x |
| file_commits | 0.518 | 0.105 | 4.9x |
| wide_commit | 0.458 | 0.185 | 2.5x |
| **total** | **5.699** | **0.721** | **7.9x** |

Almost none of that is the commit.

**Where that goes is TDS logins, and they are the connection pool warming up.** From the server's
`Logins/sec` counter, a bare attach performs three (one with `catalog false`, none also with
`lazy_validation true`), and one login on loopback is 0.175s — three of them being the 0.51s the
attach measures. But they are paid **once**: a session doing 41 commits performs the same four
logins as one doing a single commit. So this is a fixed startup cost that the pool then serves the
whole session from, not a per-operation one, and the 31x and 23x above describe *starting up*
against postgres rather than running against it. Postgres is not doing the same work more cheaply;
it defers connection setup and pays it later.

Two things follow. The benchmark's totals overstate the practical gap for a long-lived session and
understate it for a short one, so attach is worth reading as its own line rather than folded into a
total. And `lazy_validation true` removes one of the logins, 0.15s, by deferring credential checking
to first use — which for a lake is the very next statement.

A preload of the catalog metadata was tried here and **dropped**: `mssql_preload_catalog` does load
33 tables and 204 columns in 7ms, but the extension already loads eagerly at attach, so there is
nothing left for a preload to save (measured: no difference beyond noise). No setting moves the
attach either — `mssql_min_connections` is already 0, and statistics off, native types off and a
larger TDS packet all measure the same. The cost is the logins, and the logins are the warmup.

The commit-path crossover, measured per commit rather than per run — ten commits of N data files in
one session, timed and divided, which is both faster and far less noisy than timing one commit per
process:

| data files in the commit | phase 1 | phase 2 | ratio |
| ---: | ---: | ---: | ---: |
| 1 | 0.0404 | 0.0543 | 1.34x |
| 8 | 0.0450 | 0.0548 | 1.22x |
| 16 | 0.0619 | 0.0624 | 1.01x |
| 24 | 0.0747 | 0.0675 | 0.90x |
| 48 | 0.1235 | 0.0872 | 0.71x |
| 128 | 0.3071 | 0.1433 | 0.47x |
| 256 | 0.5735 | 0.2311 | 0.40x |

Phase 1 grows with the number of files, phase 2 barely does — which is the shape the design predicted
and D5 could only assert. **The crossover is around 16 data files**, and that is the threshold the
path-picking rule should use. The staged file count is local duckdb state, known before anything
crosses the wire.

### D9 — three ways the SQL Server side might have been made faster, measured

All three came out of asking what the server itself could be told to do differently. Two are
answered and the third is blocked; none of them is why the backend is slower.

**The catalog's primary keys are all CLUSTERED, and it does not matter.** SQL Server makes a
PRIMARY KEY the clustered index unless told otherwise, so DuckLake's own keys decide the physical
order of 31 indexes — and four tables key on a `VARCHAR(200)`, giving clustering keys of 200-224
bytes that every nonclustered index carries as its row locator. Declaring them `NONCLUSTERED`
instead, leaving the tables as heaps, measured **1.02x** over the whole workload and 1.00-1.09x on
every phase. Physical order is not the constraint here.

**Waiting for the log is not it either.** `DELAYED_DURABILITY = FORCED` lets a commit return before
its log record reaches disk — never a setting to ship, but a clean diagnostic. The commit phases did
not move (4.801s against 4.465s in the same conditions, which is noise). Whatever the commits are
waiting for, it is not the transaction log.

**A narrow IDENTITY primary key is the promising one, and it is blocked upstream.** The idea is not
about physical order but about the shape of the row identity: the mssql extension builds one out of
the primary key, so a composite key becomes a multi-column predicate in every statement that
identifies a row, and a 224-byte identity is expanded into every pushed-down filter. A `BIGINT
IDENTITY` primary key with the natural key kept as a `UNIQUE` constraint would make that eight
bytes.

It cannot be done today. An IDENTITY column is invisible to DuckDB — the extension's column
metadata does not read `sys.columns.is_identity` — so DuckDB counts it among the insertable columns
and rejects `INSERT ... VALUES` without a column list, which is the form all 25 of DuckLake's
catalog inserts use: *table ducklake_table has 9 columns but 8 values were supplied*. An explicit
column list works, and raw T-SQL through `mssql_exec` works, so the fix is small and narrow, and is
filed as hugr-lab/mssql-extension#327. Worth revisiting when that lands.

### D10 — the read path, profiled at production scale

`make bench-scale` at 1000 tables over 10 schemas, 41 columns each, three tables carrying a
thousand file-backed commits and one carrying a hundred schema changes: **2.36x postgres overall**,
down from 7.9x on the small workload. Fixed overheads stop dominating once the catalog is real,
which is the first thing worth knowing.

The read phases from that run:

| read | mssql | postgres | ratio |
| --- | ---: | ---: | ---: |
| a table with 100 schema versions | 23.718 | 7.899 | 3.00x |
| reattach against the full catalog | 4.523 | 0.242 | 18.7x |
| one wide table out of a thousand | 3.059 | 0.176 | 17.4x |
| filtered, over 1000 data files | 0.140 | 0.013 | 10.8x |
| all of a table over 1000 data files | 0.081 | 0.033 | 2.45x |

**Pruning over a thousand files is not the problem** — that read costs 81ms against 41,000 stats
rows. The expensive reads are the ones that touch the catalog as a whole.

Profiling one, statement by statement from `dm_exec_query_stats`, gives a single answer. Every read
session spends **480ms of server time and 264,000 logical reads in one query** — the mssql
extension's bulk metadata load — which is **76% of all server-side time on the read path**. Nothing
DuckLake asks for comes close.

What it is loading is the surprise:

| | tables | columns |
| --- | ---: | ---: |
| inlined-data tables | 1155 | 50,035 |
| catalog tables | 28 | 182 |

**1054 of those 1155 inlined tables are empty**, left behind by `ducklake_flush_inlined_data` after
it moved their rows into parquet, and all 1154 are still registered in
`ducklake_inlined_data_tables`. Taking the query text from the plan cache and alternating the two
variants over four rounds:

| | rows | seconds |
| --- | ---: | ---: |
| every table in the schema | 50,217 | 0.226 - 0.237 |
| only the 28 catalog tables | 182 | 0.017 - 0.020 |

**Thirteen times, and it is entirely volume.** Not the query's shape: removing its `ORDER BY` saves
0.04s, removing the `sys.partitions` join 0.05s, and swapping that join for the per-row
`OBJECTPROPERTYEX` the newer upstream source uses changes nothing measurable (0.254 against 0.248).
`SET mssql_enable_statistics = false` does not move the read either - 1.35s against 1.36s over the
whole session - so the cardinality the metadata carries is not what it costs.

An earlier version of this section said five times, from 640ms against 126ms. That pair was measured
in one order without alternating, so the first query paid for a cold cache and the second did not.
The same mistake the main benchmark alternates its arms to avoid, made again in a throwaway script;
the numbers above replace it.

DuckLake's own cleanup does not reach them. `DropEmptySupersededInlinedTables` selects tables whose
`schema_version` is below the newest for that table — *superseded* ones. A table emptied by a flush
is not superseded: it is the current version, and it is empty. So it survives every cleanup, and its
metadata is re-read on every session that opens the catalog.

That makes the read-path lever a cleanup rather than an index or a query rewrite, and it is worth
roughly thirteen times the biggest item on the path. Where it belongs — DuckLake's flush, a maintenance
function, or this manager - is the open question; doing it from the manager means dropping a table
DuckLake still has registered, which is DuckLake's invariant to hold, not ours to break.

### D11 — repeated reads on one connection, which is what a read workload actually does

D10 measured the read path one session at a time, which charges every read the whole cost of opening
the catalog. A read-heavy service does not work that way: it holds the connection and reads over and
over. Timing each read inside one session, against the same production catalog:

| read | mssql | postgres | ratio |
| --- | ---: | ---: | ---: |
| first read of the session | 0.686 | 0.149 | 4.6x |
| the same table again | 0.026 | 0.013 | 2.0x |
| the same table a third time | 0.027 | 0.013 | 2.1x |
| first read of a second table | 0.101 | 0.049 | 2.1x |
| that second table again | 0.026 | 0.012 | 2.2x |
| first read of a third table | 0.105 | 0.060 | 1.8x |
| first read of a fourth | 0.133 | 0.083 | 1.6x |
| back to the first table | 0.031 | 0.015 | 2.1x |

**In the steady state the backend is 2x postgres, not 8x and not 20x.** A repeated read costs 26ms
against 13ms, and touching a table for the first time costs about 0.1s against 0.05s. Everything
above those numbers is the session's first read - 0.686s against 0.149s - which is where the catalog
metadata load lands, and it is paid **once** however many reads follow.

Two things follow, and they point in different directions:

- **For a service holding its connections, the read path is close to fine.** The one-off is
  amortised to nothing over a few hundred reads, and what remains is a 2x that is spread evenly
  across every shape of read rather than concentrated anywhere fixable.
- **For anything that opens a connection per query** - serverless, a CLI, a scheduler starting a
  fresh process each time - that 0.54s of one-off is paid per query, and then the 4.6x is the real
  number. That is the case where D10's leftover empty inlined tables matter, because they are 13x of
  the metadata that one-off consists of.

So the read-path answer is conditional on the deployment, which is worth saying plainly rather than
quoting a single ratio: pool the connections and the gap is 2x; do not, and it is 4.6x with a clear
cause and a known fix that lives in DuckLake rather than here.

### D12 — why a table with a hundred schema changes takes 23 seconds to read

The slowest read in the production run was not the deep one. Reading a table with 154 schema
versions costs 23.0s against postgres's 7.9s, where reading a table across a thousand data files
costs 0.08s. Profiled, it is one statement:

| calls | server ms | statement |
| ---: | ---: | --- |
| 154 | 7382 | the full column list of the catalog, `SELECT ... FROM ducklake_column` |
| 154 | 150 | `ducklake_inlined_data_tables` |
| 154 | 12 | `ducklake_schema_versions` |
| 154 | ~4 each | sort_expression, partition_column, macro_parameters, macro_impl |

**Once per schema version, DuckLake loads the entire catalog.** `DuckLakeCatalog::GetSchemaCacheEntry`
caches on `SchemaCacheKey(snapshot.schema_version)` and the entry holds the whole catalog at that
version, so a table with 154 versions touches 154 distinct keys, misses each once, and pays 154 full
loads. Exactly 154 calls were observed, so the cache is working perfectly - the granularity is the
cost, not a bug.

The waste is in what each load reads. To interpret rows written under version V it needs that one
table's columns at V - 41 rows. It reads every table's columns at V - 41,122 rows. A thousand times
more, 154 times over.

Three things it is **not**, each measured rather than assumed:

- **Not a missing pushdown.** DuckLake's query is filtered, but the column-side predicate sits in an
  `OR column_id IS NULL` beside a LEFT JOIN, so it is a post-join predicate and nothing reaches the
  scan. Pushing it down anyway would not help: at the latest snapshot the predicate keeps 41,069 of
  41,122 rows, and the filtered scan measured slightly slower than the unfiltered one.
- **Not statistics.** `SET mssql_enable_statistics = false` measures the same read.
- **Not our SQL routing.** The postgres manager overrides `Execute`, `GetLatestSnapshotQuery` and
  `GenerateFileColumnStatsCTEBody` to go through `postgres_query`, but its `Query()` calls the base -
  so it reads the catalog through DuckDB exactly as this manager does. Copying those two overrides
  is worth about a millisecond here: the latest-snapshot query is one call at 1ms, and the stats CTE
  would put several `mssql_scan` calls in one plan, which v0.2.5 cannot materialise inside a
  transaction anyway.

Backend-independent, and visible on postgres too at 7.9s. It belongs upstream in DuckLake, as a
per-table schema load for this path rather than a whole-catalog one.

### D13 — the steady-state 2x is the catalog path, not the wire and not metadata volume

D11 left a 2x on repeated reads unexplained, and D10's answer - metadata volume - only covers the
first read of a session. Taking the rest apart:

**A round trip is cheaper here than on postgres.** Twenty trivial scans, mean per scan: **1.0ms on
mssql, 2.3ms on postgres**. So the protocol is not the gap, which rules out the obvious suspect.

**Reading through DuckDB's catalog costs four times what the same rows cost directly.** The same
predicate over `ducklake_data_file`, alternated six times each:

| | mean |
| --- | ---: |
| `SELECT ... FROM srv.dbo.ducklake_data_file WHERE ...`, the way DuckLake reads | 10.0ms |
| `mssql_scan(srv, 'SELECT ... WHERE ...')`, the way the postgres manager reads its hot queries | 2.5ms |

DuckLake issues about four catalog queries per read - the latest snapshot twice, the file list and
the record counts - so that 7.5ms of per-query overhead is most of the 26ms a repeat read costs.

**This corrects D12.** There I priced `GetLatestSnapshotQuery` at about a millisecond and concluded
that copying the postgres manager's overrides was not worth doing. That number came from
`dm_exec_query_stats`, which measures **server** time; the overhead is on the client - binding, the
catalog entry lookup, and materialising the scan inside the transaction - and is invisible there. I
measured the wrong side of the wire and drew the wrong conclusion from it.

So `GetLatestSnapshotQuery` is worth doing, on the same grounds the postgres manager does it. It is
now done, and measured on a 200-table catalog, A/B on the same data with the build swapped under it:

| | first read | mean of five repeats |
| --- | ---: | ---: |
| through `mssql_scan` | 0.253s | 0.0050s |
| through the catalog | 0.267s | 0.0056s |

**About ten percent of a repeat read, and inside the noise on the first** - much less than the 4x the
per-query comparison suggested, because this is one of roughly four catalog queries a read makes and
a repeat read on this catalog is 5ms rather than the 26ms the production one costs. The saving
should grow with the catalog, since that is what makes the catalog path expensive, but that is an
expectation and not a measurement.

**The stats CTE was tried and reverted.** It is the one that carries rows - one per file per
filtered column, so a thousand-file table contributes a thousand rows per column to every read that
prunes - and the ceiling is real: the same filtered read, hand-written both ways against a table of
301 files and 12,341 stats rows, returns the same 114 rows in 0.228s through the catalog and 0.107s
as one server-side join, and warm 0.005s against 0.001s.

The constraint is finer than "inside a transaction it fails", which is what the notes said. Measured:

| | one `mssql_scan` beside a catalog scan | two `mssql_scan`s |
| --- | --- | --- |
| autocommit | works | works |
| explicit transaction | works | fails |

So the override was written to emit the direct form only in autocommit and the catalog form
otherwise. It still fails, and the reason is the guard rather than the rule: DuckLake reads its
metadata on its own internal connection, inside its own transaction, so the autocommit the manager
can see is not the autocommit the extension decides on. Adding a second catalog scan to the
statement - the file list joins `ducklake_data_file` and `ducklake_delete_file` - is enough to
collide even with a single `mssql_scan`.

**And the size the ceiling was measured at was too small to judge by.** Synthesising file and stats
rows straight into the catalog under an unused `table_id` - the queries under test touch only those
two tables, so nothing else needs to exist - the gap grows with the catalog rather than staying
where the 301-file table left it:

| files | stats rows | filter returns | through the catalog | one server-side scan | ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1,000 | 41,000 | 6 rows | 0.007s | 0.005s | 1.40x |
| 10,000 | 410,000 | 6 rows | 0.043s | 0.014s | 3.07x |
| 30,000 | 1,230,000 | 11 rows | 0.038s | 0.005s | **7.0x** |
| 30,000 | 1,230,000 | 30,000 rows | 0.039s | 0.035s | 1.1x |

The last two rows are the same catalog and the same query differing only in how much the filter
throws away, and together they say what actually drives this: **the catalog path costs what the
catalog holds, the server-side form costs what the answer holds.** Through the catalog every stats
row crosses the wire and the join happens on the client, so 0.04s whether eleven rows survive or
thirty thousand. Server-side the join happens where the rows are and only the survivors travel.

So it widens with catalog size *and* with selectivity, and a lake read is normally selective - that
is what file pruning is for. It is worth nothing on a read that keeps everything.

`GetFilesForTable` is virtual, so the whole query could be generated as one server-side statement
and sidestep all of this. What stops that today is not the cost of the copy but its shape: of the
six pieces it is built from, five are **private** - `ReadDataFile`, `ReadDeleteFile`,
`GetFileSelectList`, `GetDeleteFileSelectList`, `GenerateFilterPushdownComponents` - and
`SetSnapshotFilter` is file-local. Only `ReadInlinedFileDeletions` is reachable. So the virtual is a
seam with nothing behind it to hold on to, and overriding it means copying private internals of the
submodule and keeping them in step, where a drift shows up not as a compile error but as a quietly
wrong file list.

Which makes the order of preference clear, cheapest first: defer `BEGIN` to the first write in the
extension, so reads take pooled connections and the constraint disappears; or ask upstream to make
those six `protected`, after which the override is small and honest; and only failing both, copy.

**`MATERIALIZED` does not rescue it either.** DuckLake already emits the
hint itself - `AS MATERIALIZED` when a CTE is referenced more than once, `AS NOT MATERIALIZED`
otherwise - so the obvious idea is to force materialisation and let the scans run one at a time.
Tried, at the top level and nested as `WITH x AS (WITH s AS MATERIALIZED (...) SELECT * FROM s)`:
both fail inside a transaction with *connection not in Idle state*, and both succeed outside one.

The reason is that `mssql_scan` runs its query **at bind time**, to learn its result columns - the
same point at which the metadata is loaded. Every source of a statement is bound before any of them
is executed, so scans in one statement collide however the plan later chooses to evaluate them.
Materialisation reorders execution, and the constraint is not in execution.

**Postgres does the same pinning and does not have the problem**, which locates the difference
precisely. The same statement - two direct scans plus a catalog scan, inside an explicit transaction
- works on postgres and fails here; and both postgres scans report the *same* `pg_backend_pid()`, so
it is one connection there too. What differs is that libpq buffers a result set fully client-side, so
the connection is idle again before the second scan binds, while the TDS path streams and leaves it
`Executing` until drained. Filed as hugr-lab/mssql-extension#329, where the closest fix is machinery
the extension already has: spec 003's R2 materialises *catalog* scans inside a transaction, and the
same treatment for `mssql_scan` would close it.

What holds the connection, and therefore what would have to change: the extension pins one pooled
connection to an explicit transaction, issues `BEGIN TRANSACTION` on it and binds the 8-byte
transaction descriptor to it, so every later statement must go there. That connection then carries
the catalog scans, our `mssql_exec` calls, and phase 2's `#temp` staging tables with the bulk loads
that fill them - session temp tables live on the connection, which is why the apply batch must run
on it.

Pinning starts at the **first access** in a transaction, read or write. A read-only DuckLake
transaction does not need SQL Server isolation at all: its consistency comes from the snapshot
predicate in DuckLake's own SQL, and writers only append new snapshots. Deferring `BEGIN` to the
first write would let reads take pooled connections and remove this constraint for them, which is
the shape most of a read workload has.

The read path therefore has two separate causes, not one:

| | cost | cause | where the fix lives |
| --- | --- | --- | --- |
| first read of a session | 0.69s vs 0.15s | metadata volume, 1054 empty leftover tables | DuckLake |
| every read after it | 26ms vs 13ms | ~4 catalog-path queries at ~10ms instead of ~2.5ms | here |

## Enforcement & security

The procedure is created by us and takes no SQL from the client: its parameters are the schema name,
a schema version, and retry numbers. The staged rows arrive as data through BCP, never as statement
text — which is the property that makes this safe where rewriting SQL was not.

Fail-open, not fail-closed: every reason to refuse the fast path (no procedure, wrong version, a
commit that is not data-only) falls back to phase 1 rather than failing the commit.

## Where this stands

Landed and exercised on every data-only commit: the staging (D1, D2), measured in D5.

Written and **correct**, but still off by default behind `MSSQL_DUCKLAKE_SERVER_COMMIT=1`: the
apply. Against the server it now runs the full cycle, and the catalog it produces is **byte-identical
to the one the client loop produces** for the same workload — the same snapshots, the same
`changes_made`, the same `row_id_start` and `next_row_id`, checked by diffing both catalogs after
running the same script on each path. The integration suite passes on both paths, unchanged.

What stood between "the batch applies correctly" and "the cycle works" was D6: the apply was not
assigning row ids. It is worth naming how that presented, because it cost a session — the commit
succeeded, our own reads of the result succeeded, and the exception arrived later from DuckLake's
cardinality estimation over parquet, three frames deep in the optimizer. The stack trace named the
cause on the first read; the guesses that preceded reading it did not.

It stays off, but no longer for want of a result: measured (D7) it is **0.53x phase 1** on a commit
carrying many data files and 1.71x on a commit carrying one small one, at 0.98x overall. What is
missing is the rule that picks between them per commit, plus the scope the apply still does not
cover (delete files, inlined data and deletes, compactions, name maps) and the server-side retry.
`ProbeServerCapabilities` does not arm it, so the default build is exactly phase 1, which stays
green — and CI now runs the suite on both paths, so the fast one cannot rot while it waits.

Two lessons already paid for, both about SQL Server rather than about DuckLake:

- a `#temp` table created inside a stored procedure dies with it, so the result table is created by
  the caller in the same batch;
- and the RETURNSTATUS desync above, which is what turned the procedure into a batch.

## Testing

- **The suite runs twice, once per path.** `make test-integration` is the default (phase 1) and
  `make test-integration-fast-path` re-runs the identical file with the apply armed; CI runs both.
  The suite drives five data-only commits through the apply, so the two paths have to agree on
  every assertion — the fast path must be indistinguishable from the slow one. This is the check
  that was missing when D6's bug landed: it was invisible to the default path and fatal on the
  other, and nothing in CI ever took the other.
- Beyond the suite, the two catalogs are compared directly: the same script run on each path, then
  `ducklake_snapshot`, `ducklake_snapshot_changes`, `ducklake_data_file` and `ducklake_table_stats`
  diffed. They come out byte-identical, for single-file commits and for a partitioned table writing
  several files in one commit. The server's plan cache confirms the batch actually ran, so an
  identical diff cannot be a silent fallback.
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
