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

### R3 — shaping (as prototyped, moved)

- Every statement that names a table is guarded by `OBJECT_ID`.
- The stamp is written last, and it also stands for the migration.
- `MigrateV10Dev`, the per-attach re-run of a dev format, returns when the stamp is current.
- `SHAPE_VERSION` moves whenever the shaping or the migration changes.

R1 moves the trigger from the attach to first use.

### R4 — reads: the catalog load is the target

The load is DuckLake's eight statements over the catalog path. Its tables+columns statement costs
85 ms on 41k column rows. DuckDB pulls those rows and builds the nested lists locally, because the
rewriter vetoes nested-type constructors. A DDL commit runs the load twice.

1. **Why twice.** Determine it from DuckLake's source before anything else. If the second load is
   a schema-cache miss that should have been a hit, the fix is upstream's, and it halves
   `create_tables` with no code of ours.
2. **The tables+columns load as our T-SQL.** Override the read through `Query`, the same seam
   specs/008 uses:
   - the joins and snapshot filters run on the server;
   - the nested parts come back as `FOR JSON`, through `mssql_scan_unsafe`;
   - DuckDB rebuilds the lists with `from_json`.

   The same result DuckLake would have assembled itself, for one server statement. We prefer the
   mssql extension to push the joins of a vetoed plan (dependency M1), so that we do not carry our
   own copy of DuckLake's load. If it cannot, we write it ourselves, behind the strict guard's kind
   of exact text match: the override recognises DuckLake's statement by its text and falls back
   when it does not match.
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
| mssql | local a71c57c | a pushed ref with 081 |
| `CMakeLists.txt` | the new `format.py` wants 80 columns | one reformat commit |
| `make tidy-check` | the pattern without the trailing slash | validated in CI |
| fast-path suite | — | in CI |
| bench catalog | in its own schema | the integration suite resets `dbo`, and it wiped the bench catalog three times |

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

1. **R4.1** — why the catalog loads twice. A diagnosis, possibly an upstream issue.
2. **R1** — first-use shaping, the patch dropped, both kinds tables, the per-format strict guard,
   and the create-at-1.1 verification.
3. **R5, bounds** — the exact comparison, once the HUGEINT question is decided.
4. **R4.2** — the tables+columns load: ours, or M1's.
5. **R5, small commits** — the procedure with the JSON payload, once M5 has an answer.
6. **R5, the threshold before staging, and schema commits.**
7. **R6** — before merge.
