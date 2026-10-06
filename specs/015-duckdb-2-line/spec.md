# Spec 015: the DuckDB 2.0 line — rework, after the prototype

- **Status**: draft. The prototype is the branch `recon/duckdb-2.0-pushdown` (never merged; its
  commits from `f68b8e1` to `5f1a236` and the probe below). It is evidence, not a base: what follows
  may rewrite any of it. The research log is `design/005-duckdb-2-bump/PORT-LOG.md` (local,
  gitignored).
- **Date**: 2026-10-06
- **Author**: hugr-lab

## Summary

The bump to the DuckDB 2.0 line brings three changes at once:

- DuckLake's format `1.1-dev1`. It adds columns and a table, renames the inlined tables' metadata
  columns, and its migration re-runs on every writable attach.
- The mssql extension's 2.0 line: TLS verification (its spec 074), the remote-pushdown rewriter
  (079/080), and given-shape and parameterised calls (081).
- A DuckLake that throws away a registered third-party manager in the attach transaction of a 1.1
  catalog.

A prototype took the manager through all three and measured everything it touched. The measurements
overturned the plan the project had been working to since specs/004. The commit is **not** where a
large catalog's time goes: a DDL commit spends 86% of its time reloading the catalog, and the commit
itself is ~15 ms of it. So this spec has three parts:

- what the prototype proved;
- what we keep, what we redo, and what we drop;
- the target design, its dependencies on the mssql extension, and an order of work in which every
  step is measured before the next one starts.

## What the prototype proved

All numbers are from the 1000-table catalog: 10 schemas, 40 columns of mixed types per table,
~41k rows in `ducklake_column`. The tool is `scripts/bench/attach_probe.py`. It puts DuckDB's
QueryLog in front of the workload, which records DuckLake's internal connection too. The pushdown
arms are alternated and a warm-up run goes first, because the first attach of a process costs 2–3x
the rest.

**One attach is 27 statements, and none of them is per table.** The catalog load is the last eight:
snapshot, schemas, tables+columns (one statement, three correlated `LIST` subqueries), views,
macros, partitions, sort keys, and table stats. After the prototype, an attach costs **385 ms with
pushdown on and 389 ms off**; 7 ms of that is the manager's. The bench's `reattach` phase takes
**0.28 s**, down from 4.45 s on and 2.77 s off.

**A DDL commit is a catalog reload, twice.** One `CREATE TABLE` (41 columns) costs ~245 ms. Of that:

| what | ms per commit |
| --- | ---: |
| full catalog load, run **twice**: tables+columns 85 + 84, sort keys 26 + 1, views 6 + 6, macros 4 + 3, schemas and partitions ~2 each | **~210** |
| creating the inlined table (ours) | 5 |
| the commit batch (ours, one `mssql_exec`) | 3 |
| two `mssql_invalidate_cache` calls | 6 |

DuckLake caches the catalog by schema version, and a DDL commit makes a new version. Both loads run
**before** the commit batch, inside the same transaction. Why there are two has not been determined
yet. This is the bench's `create_tables` phase: 169 s for 1000 tables, the largest line of the
benchmark. **A server-side commit cannot touch it.**

**A data commit is five round trips.** A file-backed insert (200 rows) costs ~19 ms in steady state:

- the latest-snapshot read and the conflict check: 4.9 ms;
- the appender into `ducklake_file_column_stats`: 3.9 ms;
- the table stats read: 3.0 ms;
- the appender into `ducklake_data_file`: 1.2 ms;
- the commit batch: ~0.8 ms;
- the rest is local: writing the parquet file.

An inlined insert (2 rows) costs the same ~19 ms. A procedure can collapse these round trips into
one.

**The pushdown rewriter is slow on one shape only**: many distinct remote scans in one plan. With
1000 distinct tables under one `UNION ALL`, each read `LIMIT 0`, planning costs 2.95 s on and 1.20 s
off at mssql `spec/079-e2`. On the mssql fix 998660e the same plan costs 0.63 s on and 0.016 s off.
The fix removed the round trips; the dry run's own CPU is still there. Exactly one statement of an
attach has this shape: DuckLake's probe of the inlined tables' column names. The prototype's own
migration no longer issues it.

**A given shape halves a scan.** The manager's latest-snapshot read, 100 calls in one transaction:

| | pushdown on | pushdown off |
| --- | ---: | ---: |
| `mssql_scan` (describes the statement first) | 155 ms | 132 ms |
| `mssql_scan_unsafe` (shape given) | 70 ms | 72 ms |

**Composition works.** Inside a transaction, these all run on the pinned connection:

- a catalog read beside an unsafe scan;
- two unsafe scans;
- a `UNION ALL` of a catalog read and an unsafe scan.

specs/008 kept every server scan as the sole source of its query because these failed. That
restriction can go.

**Defects found by the prototype:**

- The 1.0 → 1.1 migration cannot run on SQL Server. DuckLake's `{IF_NOT_EXISTS}` placeholder has no
  T-SQL form, so the first column that already exists fails with `error 2705`, and the error is
  swallowed into a warning.
- The shaping assumed its own format and died on a 1.1-only table (`error 4902`).
- The server-side commit merged per-table bounds as text: `max '9'` for values 0..49. DuckDB built a
  perfect-hash aggregate on that bound, and the query failed.
- The server-side commit's size threshold is checked after the local staging. Armed, it costs 2x on
  small commits: `second_commits` went from 21 s to 43 s.
- `make tidy-check` matches no file. ci-tools' pattern is `src/.*/`, and our `src/` is flat.
- `make test-integration-fast-path` is not run in CI. That is how the bounds bug survived.

## Prototype → rework: kept, redone, dropped

| prototype | verdict | why |
| --- | --- | --- |
| our own 1.0 → 1.1 migration in T-SQL (overriding the `Migrate*` virtuals, every step guarded, the rename all-or-nothing) | **keep** | it is the only way to upgrade a catalog; DuckLake's own SQL is never edited |
| shaping guarded by table existence rather than by version | **keep** | the version says what should exist; the server says what does |
| the shape stamp also standing for the migration (`SHAPE_VERSION` 6) | **keep** | 4.45 s → 0.28 s per attach |
| all six manager scans on `mssql_scan_unsafe` | **keep** | half the cost of every scan |
| `given_shape.test`, `migrate_v11.test`, the bounds regressions | **keep** | |
| TLS in the test DSN (`TrustServerCertificate=yes`) | **keep** | mssql's spec 074 refuses an unverifiable certificate |
| the probe and `metadata-log` taking the DSN from the environment | **keep** | every finding above came from them |
| strict-batch guard scoped to `DUCKLAKE_LATEST_VERSION`, a kinds table for 1.1 only | **redo** (R1) | with both formats supported, existing 1.0 catalogs would silently lose the T-SQL batch |
| typed bounds via `TRY_CAST(… AS DECIMAL(38,10))` | **redo** (R5) | it rounds at the tenth digit, so a bound can come out tighter than the data — a wrong bound |
| the ducklake patch: `CreateVersionedManager` hook plus a header-only `DuckLakeMetadataManagerV1_1` | **drop** (R1) | the manager stops depending on surviving the attach |
| mssql pinned to a local commit (a71c57c) | **temporary** | waits for a pushed ref |
| phase 2's `#temp` + BCP apply as the road to speed | **demote** (R5) | it addresses ~15 ms of a 245 ms DDL commit; it stays for large data commits only |

## Target design

### R1 — registration and formats: both 1.0 and 1.1, no patch

**How the 2.0-line ducklake works.**

- A manager is created **per transaction**, from the registry, by `MetadataType()`. The factory is
  not told the version, and it should not be: the version belongs to the catalog.
- Everything that depends on the version is driven from `catalog.SupportsV1_1Metadata()`, at 28
  sites — never from the manager's class.
- The manager's class matters for exactly seven virtuals, and only when a catalog is created: six
  catalog DDL statements, plus `GetVersionString()`.

**Where the swap happens, and where it does not.** `SetVersionedMetadataManager` replaces a class
it does not know with a stock manager. It does that only at format 1.1, and only in the attach
transaction. The migrations run on our manager before the swap. Every later transaction gets our
manager back from the registry.

**The rule, therefore: the manager does nothing in the attach that it depends on.** The collation
probe and the shaping move from `InitializeDuckLake` and `ProbeServerCapabilities` to the **first
use** in a transaction of ours, behind the stamp:

- one `mssql_scan_unsafe` of the stamp, per process and per catalog;
- `EnsureCatalogShape` when the stamp is behind.

At 1.1, the stock manager creates a catalog with stock DDL, and our shaping converts it. Converting
a catalog created by someone else's DDL is the job the shaping was written for.

**To verify first.** The stock 2.0-line `InitializeDuckLake` must be able to create a catalog
through the 2.0-line mssql writer (its spec 080). One integration run on an untouched ducklake
answers that. Fallback if it cannot: create new catalogs at 1.0 by giving
`ducklake_default_version` a default.

**The commit batch must recognise both formats.** At 1.0, the kinds table is the 1.1 one without
the trailing columns of five tables: `data_file`, `delete_file`, `file_column_stats`,
`table_column_stats` and `schema`. The strict guard checks against the catalog's own format, so an
unrecognised statement is an error at either format. A 1.0 catalog keeps the T-SQL batch it has
today.

**Upstream.** Throwing away the registered manager is upstream's bug, and ducklake#1066's libSQL
manager hits the same problem. The fix is one line: leave alone a manager whose
`GetVersionString()` already equals the requested version. To be proposed, without urgency;
nothing here depends on it.

### R2 — the migration (as prototyped)

The order of the steps:

1. The inlined data tables' metadata columns are renamed to `_ducklake_`, in one transaction under
   `XACT_ABORT`. The list comes from `ducklake_inlined_data_tables`. The inlined deletion tables keep
   their names.
2. Columns are added behind `COL_LENGTH` guards, in this catalog's types.
3. `ducklake_view_column_tag` is created behind `OBJECT_ID`.
4. The version is moved, from `1.0` or `1.1-dev1` only.
5. The stamp is dropped.

Two T-SQL facts cost a round each in the prototype:

- `SELECT DISTINCT @v += …` has no defined result. The batch uses `STRING_AGG` over a `DISTINCT`
  derived table instead.
- Joining a BIN2 `VARCHAR` to `sysname` needs `COLLATE DATABASE_DEFAULT`.

The upgrade is explicit: `ATTACH … (AUTOMATIC_MIGRATION TRUE)`. A plain attach of a 1.0 catalog
stays at 1.0, and that is supported (R1).

No performance work is planned for the migration: it runs once per catalog, when the format
changes. The only cost that matters here is DuckLake *calling* it again on every writable attach
while the format is `1.1-dev1`. That is attach cost, not migration cost, and R3's stamp check makes
the repeated call return at once. When 1.1 is final, the repeated calls stop.

### R3 — shaping and the migration marker: two stamps, read together

- Every statement that names a table is guarded by `OBJECT_ID`.
- **The shape stamp** (`mssql_ducklake_shape` = `SHAPE_VERSION`) is written last by the shaping, and
  it means only that: the catalog has this build's keys, collations and indexes. R1 moves the
  shaping's trigger from the attach to first use.
- **The migration marker** (`mssql_ducklake_migration` = the format plus the revision of our
  migration, e.g. `1.1-dev1/1`) is written as the last step of our migration. On every writable
  attach of a dev-format catalog, `MigrateV10Dev` compares the marker with the build's value. If
  they are equal, it does nothing. If not, it runs the migration (idempotent) and rewrites the
  marker. The revision exists because upstream can add a column under the same `1.1-dev1` name:
  raising the revision brings already-migrated catalogs along, and only them.
- Both are extended properties on `ducklake_metadata`, which DuckLake never reads. A row in its
  key/value table would not do: DuckLake reads every row of that table on every attach. Both
  properties are read **in one statement**, the one that reads the stamp today, so the check costs
  no extra round trip.
- The prototype tied the two into one stamp (`SHAPE_VERSION` 6 meant "shaped and migrated"), which
  forced the rule "move `SHAPE_VERSION` when the migration changes". With two markers that rule is
  gone: the shaping and the migration move independently.

### R4 — reads: the catalog load is the target

The load is DuckLake's eight statements over the catalog path. Its tables+columns statement costs
85 ms on 41k column rows. DuckDB pulls those rows and builds the nested lists locally, because the
rewriter vetoes nested-type constructors. A DDL commit runs the load twice.

1. **Why twice — answered, and both halves are upstream's.** Traced on the 1000-table catalog by
   DuckDB connection and query id (`attach_probe.py`, `duckdb_logs.connection_id` /
   `query_id`). Each `CREATE TABLE`'s metadata transaction runs:
   - **the load at its start snapshot N.** The previous commit created schema version N, and
     DuckLake's cache, keyed by schema version, holds only the previous one, so it misses. That
     cache is not filled from the committed state after a commit; filling it is upstream work,
     close to incremental loading;
   - **a full build at the commit snapshot N+1, before N+1 is written.** `ducklake_transaction_state.cpp:1788`:
     a commit that creates tables calls the static
     `DuckLakeMetadataManager::BuildCatalogForSnapshot` — every table, column, view, macro and sort
     — and uses **only `existing_catalog.partitions`** from it, for `WriteNewPartitionKeys`. About
     105 ms of the ~245 ms. Because the function is static, it bypasses the `GetCatalogForSnapshot`
     virtual: a prototype that reused the previous load through that virtual never fired, which is
     how this was found (eight virtual calls, twelve executed loads).

   **Nothing safe to do here.** Its statements reach our `Query` exactly like a real load. Answering
   them "empty except partitions" would make correctness depend on upstream reading nothing else
   from that object. The upstream fix is small and measured: query the partitions, not the
   catalog. Filing it is the owner's decision (it is a third-party repository). A `DROP TABLE`
   commit makes no such call: one load per commit.
   **And a third place, measured after: the flush.** `ducklake_flush_inlined_data` costs ~214 ms a
   table on the 1000-table catalog, and ~165 ms of that is a full catalog load **per table**. Each
   table's flush asks for its schema version's `begin_snapshot`
   (`SELECT begin_snapshot FROM ducklake_schema_versions WHERE table_id = ? AND schema_version = ?`)
   and loads the whole catalog at that historical snapshot, to read the inlined rows with the schema
   they were written under. Tables created in different schema versions share no cache entry. This
   is upstream's per-schema-version whole-catalog load (specs/005 D12), and it is the bench's
   `flush_inlined` phase (135–144 s).

   **So the cost to attack is one catalog load, ~165 ms here.** `create_tables` pays it twice per
   table, the flush once per table, an attach once.

2. ~~**The tables+columns load as our T-SQL.**~~ **Measured, and not worth it.** The catalog in
   question has ~41k rows in `ducklake_column`, and the time is mostly moving them over TDS:

   | what is read | ms |
   | --- | ---: |
   | DuckLake's tables+columns statement, catalog path (in a transaction, as a commit runs it) | 76–82 |
   | the same tables × columns as one T-SQL join, without the tags | 80–93 |
   | the same through T-SQL with `FOR JSON` for the nested parts | 312–337 |
   | all of `ducklake_column` through the catalog path, nothing else | 56–62 |

   (A first measurement that wrapped the statement in `SELECT count(*)` read 22–33 ms. That was the
   optimizer dropping the correlated subqueries and the unused columns; the numbers above
   materialise the full result.) Three quarters of a load is the transfer of the column rows. No
   rewrite of the statement on our side can beat it, and the server-side join with JSON is four times
   slower. What helps is *not loading*, and all three avoidable loads are upstream's. M1 loses its
   point with this.
3. **Composition.** The file-column-stats CTE from specs/008 becomes a direct scan.
4. **Later:** all eight load statements from one call, when multiple result sets exist (M2).

Incremental loading — only what changed since the cached schema version — is upstream work, and
the real answer at large scale. It is noted, not planned.

### R5 — the commit

**Bounds, exact.** The per-file bounds (`ducklake_file_column_stats`) are what file pruning reads,
and they are inserted verbatim. The per-table bounds (`ducklake_table_column_stats`) are what the
planner reads, and when the server applies a commit, the server merges them. The merge must
compare exactly, in the column's order:

| DuckLake type | compare as |
| --- | --- |
| `int8…int64`, `uint8…uint32` | `BIGINT` |
| `uint64` | `DECIMAL(20,0)` |
| `decimal(p,s)` | `DECIMAL(p,s)`, from the type string |
| `float`, `double` | `FLOAT(53)`: DuckLake writes 17 significant digits, so the text round-trips; scientific notation parses too |
| `hugeint`, `uhugeint` | open — `DECIMAL(38,0)` stops at ±10³⁸, and HUGEINT goes up to ±1.7·10³⁸ |
| dates, timestamps, booleans, strings, blobs, uuids | BIN2 text order, which is already DuckDB's |

A value that cannot be compared exactly leaves the per-table bound NULL. DuckLake reads NULL as "no
statistics": file pruning is not affected, and only the table-level bound is lost. A guessed bound
is never written.

**Measured (2026-10-06): the merge goes to the client.** Both merges were built behind
`MSSQL_DUCKLAKE_STATS_MERGE` (`client` by default, `server`) and compared:

- **Correctness.** One workload of edge cases went through the client loop and through both merges:
  negatives of differing lengths, a 128-bit HUGEINT, doubles in exponent form, dates, timestamps,
  `timestamptz`, an all-NULL column turned sticky-unknown, a partitioned multi-file commit. The
  stored per-table stats were byte-identical in all three. The client merge matched on its first
  run. The server merge needed a second: the first run wrote `0` for an unknown NaN flag and
  exactness for an absent bound — the second copy of the rules drifting, which is exactly the risk.
- **Speed.** The 1000-table bench with the apply engaged on every commit
  (`MSSQL_DUCKLAKE_SERVER_COMMIT_MIN_FILES=1`); `second_commits` is 1000 data commits of one file
  each:

  | | client loop | apply, client merge | apply, server merge |
  | --- | ---: | ---: | ---: |
  | `second_commits` | 21.3 s | 77.8 s | 166.8 s |
  | total | 491.9 s | 552.2 s | 637.5 s |

  The server merge costs ~90 ms more per commit than the client merge for the same apply.

The decision is the client merge, and the server merge's code goes.

**The `#temp` apply itself is 3.6x the client loop on a one-file commit**, and this breakdown is
why the small-commit design below has no temp tables at all (applied commit ~76–96 ms against
~19 ms):

| what | ms per commit |
| --- | ---: |
| DuckLake's local staging: one `INSERT` per column (41) into duckdb temp tables | 16.4 |
| three `COPY … TO` — one BCP per staged table | 24.0 |
| the apply batch | 18.5 |
| reading the result back from `#temp` | 6.4 |
| the latest snapshot and the conflict check | 8.0 |
| the lock and the stats read (client merge) | 5.1 |

**What is left of a data commit, measured after D7.1 and R5.** A one-file data commit costs ~21 ms
in steady state, the same with the appender on or off. With it off, the appender's two round trips
(6 ms) go, but DuckLake adds a schema/table path lookup (3 ms) for the `INSERT … VALUES`. What
remains are three reads DuckLake's protocol makes on the client: the transaction's snapshot, the
conflict check with the stored stats, and the paths. Then the batch, and ~8 ms of local work
(writing the parquet file). With the merge on the client (above), a server procedure cannot remove
those reads. **The small-commit procedure is worth a few milliseconds at most and drops in
priority**; it still waits on M5.

D7.1 is done: with the apply armed at its default threshold, `second_commits` is 20.8 s, against
21.3 s for the client loop and 43 s for the prototype.

**Small commits: one call, no temp tables.** Most commits are small. A data commit's five round
trips become one:

```
mssql_scan_params_unsafe(ctx, 'EXEC dbo.ducklake_commit @payload = @p', {'p': <json>}, columns := {…})
```

- The payload carries the commit's rows: the data file, a stats row per column, the snapshot and
  its changes. The server reads it with `OPENJSON`, which needs SQL Server 2016; the catalog already
  needs 2019.
- Inside the procedure: snapshot allocation, the conflict check, the typed bound merge, and the
  retry.
- The snapshot id comes back in the same round trip. That also removes the separate `#temp` read
  and the latest-snapshot scan.

This depends on M5: the scan must run exactly once. The JSON payload stands in until TVPs exist
(M3). Measured target: the 19 ms data commit, minus the round trips it saves.

**Large commits** keep the prototype's `#temp` + BCP apply, which was measured at 0.40x at 256
files. Its threshold is now decided **before** staging: the count is in
`LocalTableDataChanges::new_data_files`.

**Schema-changing commits** go through the same procedure once data commits are measured. They are
small in rows. Their cost is the catalog reload (R4), not the commit, so they come after R4.

### R6 — pins, tooling, CI

| piece | rework | before merge |
| --- | --- | --- |
| duckdb | the 2.0 line | a released tag |
| ducklake | **untouched** (R1) | the bump's SHA |
| mssql | hugr-lab/mssql-extension#407 head 9f369d5 (pushed; 081 on top of main 88fe137) | a release tag |
| `CMakeLists.txt` | the new `format.py` wants 80 columns | one reformat commit |
| `make tidy-check` | the pattern without the trailing slash | validated in CI |
| fast-path suite | — | in CI |
| bench catalog | in its own schema | the integration suite resets `dbo`, and it wiped the bench catalog three times |

## Against postgres, measured (2026-10-06)

The same reduced bench (1000 tables, 10 schemas, 40 columns, no deep history, no partitioned tables)
on both backends in one run. postgres_scanner is built from duckdb-postgres `main` (no binary exists
for a dev duckdb), with one call patched for our duckdb pin.

| phase | mssql | postgres | mssql / postgres |
| --- | ---: | ---: | ---: |
| `create_tables` | 179.3 | 186.7 | 0.96 |
| `first_commits` (inlined inserts) | 25.8 | 18.2 | **1.42** |
| `second_commits` (file-backed) | 21.9 | 63.7 | 0.34 |
| `flush_inlined` | 147.6 | 137.1 | 1.08 |
| `merge_adjacent` | 9.3 | 30.1 | 0.31 |
| `partitioned_merge_adjacent` (a merge over the whole lake here) | 119.1 | 82.2 | **1.45** |
| `filtered_read` | 0.44 | 0.22 | **2.01** |
| `table_info` / `cleanup_files` / `deep_read_filtered` | 0.06 / 0.10 / 0.09 | 0.01 / 0.005 / 0.01 | **5–20** |
| reattach phases | 0.4–0.7 | 0.9–1.3 | 0.35–0.72 |
| **total** | **507.5** | **522.4** | **0.97** |

**On this line the mssql backend is no longer the slower one overall.** The two largest phases,
`create_tables` and `flush_inlined`, are at parity. Both are dominated by upstream's catalog loads,
and the postgres manager pays those loads exactly as we do: its reads go through the base path.

Where postgres is still ahead:

1. **inlined inserts**, ~7.6 ms a commit;
2. **a merge of adjacent files over the whole lake**, 37 s;
3. **the read path** — closed since. The postgres manager overrides `GenerateFileListQuery` and the
   file-column-stats CTE and runs the whole file-list query natively in postgres through one
   `postgres_query`; ours went through the catalog path piece by piece. Now ours does the same: the
   file list is DuckLake's query, its casts and the few non-T-SQL spellings rewritten (`TRY_CONVERT`,
   `COALESCE` extremes for `MIN`/`MAX`, `contains_nan`, `IS DISTINCT FROM`, `NULLS LAST`), sent as one
   `mssql_scan_params_unsafe` with the shape given. A filter it does not cover goes to the catalog path
   (strict mode: an error naming it); `MSSQL_DUCKLAKE_NO_SERVER_FILE_LIST=1` forces the catalog path.
   Every constant, the table id, the column ids and the snapshot are **parameters**, so the text is
   one per filter shape, whatever the table or the value, and no literal has to survive the
   database's code page (a `раздел3` literal did not, before). Measured, 500-table lake, a
   range read of one table: catalog path 13 ms, server statement 9–11 ms (postgres: 10 ms). Over 40
   reads of different tables with different bounds the server holds 2 prepared plans used 80 times —
   identically under `PARAMETERIZATION SIMPLE` and `FORCED`, so the read no longer depends on
   specs/012's best-effort option. `read_pruning.test` pins the pruning type by type, file against file.

The errors in both runs' logs are the reduced configuration's (queries for partitioned and deep
tables it was told not to make), identical across every run of the day and on both backends.

## Dependencies on the mssql extension

Sent to the mssql session on 2026-10-06, ranked by the measurements above.

| | need | for |
| --- | --- | --- |
| M1 | push the joins and filters of a plan vetoed for its nested-type constructors, and leave only the `LIST`/`STRUCT` building to DuckDB | R4.2, without our own copy of the load |
| M2 | several result sets from one call | R4.4: the eight load statements in one round trip |
| M3 | real TDS RPC: TVPs, binary and `OUTPUT` parameters, a procedure's return code. Today parameters travel as text in a `DECLARE` line | R5: commit rows without JSON |
| M4 | `mssql_exec_params` that sends many rows in one batch, instead of one batch per row | staging |
| M5 | a guarantee that a side-effecting `*_unsafe` scan runs exactly once — or an `mssql_exec` that returns rows | R5: the commit procedure returning its snapshot |
| M6 | the dry run's CPU per node; `mssql_invalidate_cache(schema)` must see a table created after the attach | reported |
| M7 | a pushed and released 081 | merge |

## Enforcement & security

- A migration step that fails surfaces as an error; nothing is swallowed into a warning. The rename
  is all-or-nothing.
- A scan whose declared shape does not match the stream fails with an error that names the
  statement. Nothing is coerced.
- A bound that cannot be compared exactly is NULL, never a guess.
- When a statement fails, the test runner echoes it **after** substitution, so the DSN, password
  included, appears in the failed run's output. Redact it before pasting a failure anywhere.

## Testing

- From the prototype, kept:
  - `migrate_v11.test`: the 1.0 → 1.1 upgrade with data; attached plain, migrated, then twice more.
  - `given_shape.test`: the type rules, both refusals, and the three composed shapes.
  - In `attach_mssql.test`: the stored bounds of a partitioned column, and a HUGEINT past the cast's
    range.
- New:
  - a 1.0 catalog writing through the T-SQL batch under the strict guard (R1);
  - a 1.1 catalog created by the stock manager, then converted on first use (R1);
  - bounds at the edges of each row of the R5 table;
  - the catalog-load override answering exactly what DuckLake's own statement answers, on the same
    catalog (R4).
- Both suites — the default and the fast path — must agree, and both run in CI.
- Every step of the work is measured with the probe, before and after:
  - `--create-tables` and `--insert-commits` (the breakdowns above);
  - `--rounds` for the attach;
  - `--shape-ab`;
  - the bench, with its catalog in a schema of its own.

## Alternatives considered

- **Patching DuckLake's migration SQL, or its versioned-manager swap.** Rejected: the virtuals and
  first-use cover both.
- **Pinning the format to 1.0.** Kept only as the fallback in R1.
- **Phase 2's `#temp` apply as the main lever.** Demoted by the DDL breakdown.
- **Shadow typed columns for the per-table bounds.** Rejected: DuckLake's positional INSERTs on the
  base path would hit columns they do not know about.
- **Merging the per-table bounds on the client.** Rejected: it costs a round trip.
- **`remote_pushdown: false` for the metadata catalog.** Withdrawn: the attach no longer has the
  statement that made it worth it.

## Order of work

Each step lands with its tests and its before/after numbers.

1. ~~R4.1~~ — answered: both loads in a DDL commit are upstream's.
2. ~~R1, R3~~ — both formats on an untouched ducklake; the migration marker (`334923a`).
3. ~~R5, the merge~~ — the client merge, measured against the server one and kept; the server one
   removed (`0f5dffb`, `68e0ff7`).
4. ~~D7.1~~ — the apply's size decided before staging.
5. ~~R4.2~~ — measured; a rewrite of the load cannot beat the transfer of the column rows.
6. **Upstream, the owner's call** — four measured issues, the largest first:
   - the flush's historical catalog load per table (~165 ms a table);
   - the full catalog build for `partitions` only in a commit that creates tables (~105 ms);
   - the reload after every DDL commit because the cache is not filled from the committed state;
   - the registered manager discarded in the attach transaction at 1.1 (a one-line fix; we no longer
     depend on it).
7. ~~Ours~~ — done or explained:
   - **the sort-keys statement is not slow.** It costs 1–3 ms at any snapshot, inside or outside a
     transaction, with pushdown on or off; the first touch in a process is ~170 ms (the
     extension's metadata of the two tables). The "~30 ms" in the breakdowns was the attribution:
     a statement's time is the gap to the next log entry, the sort keys are the last statement of a
     load, and what follows them is DuckLake building the catalog's objects in memory from ~41k
     column rows (~28 ms). That is DuckLake's, not the server's and not ours;
   - **the inlined-table creation stays where it is** (5–14 ms): on a connection of its own, outside
     the transaction, because a commit that creates a table and inlines rows into it (any small
     `CREATE TABLE … AS`) needs the extension to see the table before the batch, and a table created
     inside the transaction holds a lock the extension's metadata read waits on;
   - **the second cache refresh is gone.** The table was refreshed right after we created it; the
     clear after the commit named it again only so as not to fall back to the whole schema. It now
     skips tables already refreshed: one round trip less per DDL commit.
8. **R6** — before merge.
