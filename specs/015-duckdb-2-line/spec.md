# Spec 015: the DuckDB 2.0 line — what the port changed in the manager, and the transition

- **Status**: draft — the work lives on `recon/duckdb-2.0-pushdown` (never merged); the research is
  `design/005-duckdb-2-bump/PORT-LOG.md` (local, gitignored)
- **Date**: 2026-10-06
- **Author**: hugr-lab

## Summary

The bump to the DuckDB 2.0 line brings three things at once: DuckLake's format `1.1-dev1` (new
columns, a new table, prefixed inlined-table columns, and a migration that re-runs on every
writable attach), the mssql extension's 2.0 line (TLS verification, the remote-pushdown rewriter of
specs 079/080, and spec 081's given-shape scans), and a DuckLake that discards a registered
third-party manager in the attach transaction whenever a catalog's format is 1.1 — which the manager
sidesteps by **no longer depending on surviving the attach** (D9): both formats, no patch, and
every catalog a release of ours created keeps working unchanged. The reconnaissance branch ported the manager through all
three, and found and fixed four defects of ours on the way — two of them release-level (the format
migration could not run on SQL Server at all; the server-side commit wrote wrong statistics).

This spec distils the decisions that are now in code on that branch, what each is worth in
numbers, the one decision that is not finished (exact bounds in the server-side commit), and the
order in which the branch becomes a release. It is a draft on purpose: the transition is the part
to think about, and the questions are listed, not settled.

## Problem

A catalog created by 0.1.x is at format 1.0. The 2.0-line ducklake wants 1.1-dev1 and migrates with
`ALTER TABLE … ADD COLUMN {IF_NOT_EXISTS}` through duckdb, which the mssql extension's ALTER has
no form for; the guard is dropped and the migration dies on the first column that already exists
(`error 2705`), swallowed into a warning. **Existing catalogs could not be upgraded.**

Three more things were wrong or unknown:

- the manager's own shaping assumed its own format, so a 1.0 catalog died on a table that only
  exists at 1.1 (`error 4902`);
- the server-side commit (specs/005 phase 2) merged per-column bounds as **text**, so fifty
  partitions of 0..49 produced `max '9'`, and DuckDB — which builds a perfect-hash aggregate from
  that bound — failed the query (`aggregate group 16 exceeded total groups 16`);
- an attach of a 1000-table catalog cost 4.45 s with the pushdown rewriter on, 2.77 s off, and
  nobody knew which statement paid it.

And the known gap: specs/008 could use a server scan only as the **sole source** of a query, because
binding one took the transaction's pinned connection and a plan with two such sources failed
depending on execution order.

## Design

### D1 — the format migration is ours, in T-SQL, and none of DuckLake's SQL is touched

The three migration virtuals (`MigrateV10`, `MigrateV10Dev`, `MigrateInlinedColumnNames`) are
overridden in `src/mssql_catalog_shape.cpp`, and the migration is written the way the shaping writes
everything: each step guarded by the server's own catalog views, so a re-run is a no-op rather than
an error. In order:

1. the inlined DATA tables' three metadata columns are renamed to 1.1's `_ducklake_` names — first,
   so a collision in a user's table aborts while the catalog still says 1.0 (upstream's order, for
   the same reason). Driven by `ducklake_inlined_data_tables`, the catalog's own registry, not by a
   name pattern; the inlined DELETION tables keep bare names (specs/006 D5b). All of the renames run
   in one transaction under `XACT_ABORT`, so a failure cannot leave a catalog half-prefixed — a 1.0
   build could not read one. `sp_rename` carries the primary key along;
2. the six columns and `ducklake_schema.parent_schema_id`, each behind `IF COL_LENGTH(…) IS NULL`,
   in this catalog's types (`BOOLEAN DEFAULT NULL` is `BIT` here);
3. `ducklake_view_column_tag` behind `IF OBJECT_ID(…) IS NULL`, with the catalog's UTF-8 BIN2
   `VARCHAR`s and `[key]` quoted;
4. the version row set to `1.1-dev1` — only from `1.0` or `1.1-dev1`, so a stray call cannot relabel
   a later format;
5. the shape stamp dropped, so `EnsureCatalogShape` on the same attach keys, collates and indexes
   what just appeared.

Two T-SQL facts cost a round each and are worth writing down: `SELECT DISTINCT @v += …` has no
defined result (it renamed nothing, silently — the batch is `STRING_AGG` over a DISTINCT derived
table now), and a join between the catalog's BIN2 `VARCHAR` and `sys.tables.name` (sysname, the
database's collation) needs `COLLATE DATABASE_DEFAULT` on the predicate (`error 468`).

The pre-1.0 migrations have the same `{IF_NOT_EXISTS}` problem and are left alone: every catalog a
release of this extension created is at 1.0 or later.

DuckLake's dispatch, for the record: a plain attach of a 1.0 catalog **stays at 1.0** (the target
resolves to the catalog's own version); the upgrade is `ATTACH … (AUTOMATIC_MIGRATION TRUE)`; and a
`1.1-dev1` catalog re-runs the migration on every writable attach, because a dev format's version
string does not move when upstream adds to it.

### D2 — the shaping is guarded by existence, not by version

Every statement of `EnsureCatalogShape` that names a table is wrapped in
`IF OBJECT_ID(…) IS NOT NULL BEGIN … END`. The version says what SHOULD be there, the server says
what is, and a half-finished migration is exactly the case where the two disagree. The dynamic
blocks (generated from `sys.columns`) were existence-safe already.

### D3 — the stamp stands for the migration as well as the shape

Because DuckLake re-runs the dev-format migration on every writable attach, D1's stamp drop made
the shaping re-shape the whole catalog on every attach: **1.8 s** for a 1000-table catalog, of which
830 ms the keys batch and 246 ms the rename batch, both arms alike. `MigrateV10Dev` now returns when
`CatalogShapeIsCurrent()`: the stamp is written after the shaping, which runs after the migration,
so a current stamp means a build wanting this shape has already applied both. The version is
`1.1-dev1` there by construction (that is the branch DuckLake took), and a 1.0 catalog arrives at
`MigrateV10`, which always runs.

`SHAPE_VERSION` is **6**, and from now on it moves when the MIGRATION changes, not only the shaping.

Measured by `scripts/bench/attach_probe.py` (new; one ATTACH with DuckDB's QueryLog in front of it,
which records DuckLake's internal connection too, both pushdown arms alternated with a warm-up):
an attach of the 1000-table catalog is **385 ms on / 389 ms off**, of which 7 ms is ours. The bench's
own `reattach` phase: **0.28 s**, against 4.45 / 2.77 before.

### D4 — the strict batch guard speaks only at the build's format

The commit batch's kinds table (specs/014) describes this build's format. A catalog left at 1.0
writes tuples of other widths; the rewrite declines them by construction and the base path carries
them — correct, just not in one round trip. `MSSQL_DUCKLAKE_STRICT_BATCH` now only errors when the
catalog is at `DUCKLAKE_LATEST_VERSION`. No dual-format batch: a 1.0 catalog is one to migrate, not
one to optimise for. The side effect is the re-audit trigger the vendoring rule asks for — at the
next ducklake bump LATEST moves ahead of the table and the suite starts naming statements.

Verified the guard still fires: a kinds entry removed, `write_shapes.test` named the statement.

### D5 — the manager's scans state their shape

mssql spec 081's `mssql_scan_unsafe(ctx, query, columns := {…})` declares the result's shape, so
bind sends nothing and takes no connection. All six of the manager's scan sites are on it: the shape
stamp, the collation probe, the conflict check (specs/007, fifteen columns), the generic server-scan
helper and its one caller (the inlined-deletion existence probe), the latest-snapshot read, and the
commit result out of its `#temp` table — the one statement `sp_describe_first_result_set` cannot
describe at all, so it stops being a special case.

Worth: on the latest-snapshot read, 100 calls in one transaction, **70 ms against 155 ms** with the
pushdown on, 72 against 132 off — the describe is half the cost of the statement, and the pushdown
arm's own difference goes with it. Per transaction and per commit, not per attach.

Two type-rule traps, both in our own SQL: `COUNT(*)` is an `int` on the server (`COUNT_BIG` is how to
mean BIGINT), and `CASE WHEN … THEN 0 ELSE 1 END` is an `int` too. A declared shape is checked
strictly at execution — a mismatch is an error naming both shapes and the statement, never a wrong
answer — and every one of our statements casts to what it declares, so the two cannot drift.

**The sole-source rule of specs/008 ends here.** `test/sql/integration/given_shape.test` pins the
three shapes the released pin could not run on the pinned connection: a catalog read beside a server
scan, two server scans, and a `UNION ALL` of the two. The file-column-stats CTE that specs/008 left
on the catalog path can be a direct scan when that is taken up (follow-up, not done).

### D6 — the server-side commit's bounds are merged in the column's order — not finished

What the apply writes is two different things, and the distinction answers the question "does a
missing bound break file pruning":

- `ducklake_file_column_stats`, **per file**, is inserted **verbatim** from what DuckDB computed.
  File pruning reads these. The merge does not touch them.
- `ducklake_table_column_stats`, **per table**, is what the apply merges itself, and it is what
  DuckDB's planner reads — the perfect-hash aggregate above, join planning. A wrong value here is a
  failed query; a NULL is a weaker plan.

The merge compares in the column's order, with the type read from `ducklake_column`: the numeric
families through a cast, everything else (ISO dates and timestamps, booleans, strings, blobs, uuids)
in the BIN2 text order, which is already DuckDB's. **A value the cast cannot read leaves the bound
NULL** rather than guessed. With the cast as written (`TRY_CAST(… AS DECIMAL(38,10))`) that is
exactly three cases, for numeric columns only: a HUGEINT of magnitude ≥ 10²⁸, a double DuckLake wrote
in scientific notation (`1e+20` — SQL Server does not parse that into DECIMAL), and `nan`/`inf`. The
MATCHED branch applies the same test to the value already stored, so a catalog whose earlier bounds
were written by the default path in a form the cast cannot read loses that table-level bound on its
first server-side commit.

**The defect that remains**: `DECIMAL(38,10)` rounds at the tenth fractional digit. Two doubles that
differ beyond it compare equal, the pick between them is arbitrary, and the bound can come out
tighter than the data by less than 10⁻¹⁰ — which is a *wrong* bound, the failure mode the NULL rule
exists to avoid. The default path compares typed values exactly; the T-SQL merge must too:

| DuckLake type | compare as | exact? |
| --- | --- | --- |
| `int8…int64`, `uint8…uint32` | `BIGINT` | yes |
| `uint64` | `DECIMAL(20,0)` | yes |
| `hugeint`, `uhugeint` | `DECIMAL(38,0)` covers ±10³⁸, HUGEINT reaches ±1.7·10³⁸ | not at the top of the range — open |
| `decimal(p,s)` | `DECIMAL(p,s)` from the type string | yes |
| `float`, `double` | `FLOAT(53)` — DuckLake writes 17 significant digits, the text round-trips | yes, when the text is decimal; scientific notation parses into FLOAT too, so this also removes the `1e+20` NULL |

This is the one decision of the branch that is **not** finished: the regression is fixed (both paths
agree, 421 assertions each), the exactness is not. The table above is the proposal.

### D7 — phase 2 (specs/005): what the measurements say, and the transition it implies

With the bounds fixed, the server-side commit armed against the 1000-table bench:

| phase | default path (three runs) | server commit |
| --- | ---: | ---: |
| `create_tables` | 165.1 / 174.4 / 168.5 | 167.9 |
| `first_commits` | 23.79 / 24.53 / 23.70 | 23.02 |
| `second_commits` | 21.24 / 21.26 / 20.12 | **43.05** |
| total | 475.0 / 488.5 / 475.3 | **492.6** |

Not the typed merge (42.5 s with the old text merge). The apply engages only above a size — 1.34x
the client loop at one data file, 0.40x at 256, crossing over near sixteen (specs/005 D5, D7) — and
that threshold is checked **after** `StageCommitLocally`, so a commit of one file pays DuckLake's
staging into duckdb temp tables and then runs the client loop anyway. A trace of ten such commits
sends no `#ducklake_staged` and no `ducklake_commit` to the server at all. And `create_tables`, the
169 s that are the point, is DDL plus a first commit per table — not `IsDataFilesOnlyCommit`, so the
apply never sees it.

The transition, in the order the measurements impose:

1. **decide before staging.** The count is in the transaction's own
   `LocalTableDataChanges::new_data_files`. Removes the 2x and makes arming the switch harmless —
   the precondition for measuring anything else honestly.
2. **a second apply for small commits, with no temp tables.** The user's proposal, and the right
   one: below the threshold the commit's rows go as **parameters** of one call — `mssql_exec_params`
   (spec 081) carries `{name: value}` pairs, and the rows of a small commit (one file row, a stats
   row per column, the snapshot and its changes) fit in a JSON parameter read server-side with
   `OPENJSON` (SQL Server 2016+; the catalog already requires 2019 for its collation). One round
   trip, no staging, no bulk load, and the retry stays on the server. The existing `#temp` + BCP
   apply remains for large commits, where it was measured at 0.40x.
3. **take the schema-changing commits.** Nothing else touches the 169 s. The small-commit call is
   the vehicle: a CREATE TABLE commit is a handful of rows into `ducklake_table`, `ducklake_column`
   and the schema-version tables — a parameterised procedure can take those as it takes the stats
   rows. This is the step that makes phase 2 pay, and it is the largest.
4. **then the procedure.** Once the two applies exist, the batch becomes `EXEC dbo.ducklake_commit`
   with the parameters, and the conflict check and retry move inside it (specs/005 D3 wanted this
   and could not have it before procedure calls).

### D8 — what the pins say, and what must change before the branch can merge

| piece | on the branch | before merge |
| --- | --- | --- |
| duckdb | the 2.0 line | a released tag |
| ducklake | main, **patched** on the branch — a 60-line hook (`CreateVersionedManager`) plus a header-only `DuckLakeMetadataManagerV1_1` so a third-party manager survives `SetVersionedMetadataManager` | **no patch**: D9 moves the attach-time work to first use, so the swap costs nothing; the hook and the template are dropped |
| mssql | **local** `a71c57c` (spec 081 2/n on top of 998660e, the pushdown fix) | a pushed ref; 081 becomes its own PR after #406 |
| `MSSQL_DUCKLAKE_TEST_DSN` | `TrustServerCertificate=yes` added by the Makefile — mssql specs/074 refuses an unverifiable certificate | stays |
| `CMakeLists.txt` | the new duckdb's `format.py` wants it at 80 columns | one reformat commit |
| `make tidy-check` | ci-tools' pattern `src/.*/` matches nothing in a flat `src/`; the code-quality job has passed vacuously since the repository started | the pattern without the slash, validated in CI |
| `make test-integration-fast-path` | not in CI, which is how D6's bug survived | in CI |

### D9 — both formats, no patch: the manager stops depending on surviving the attach

How the 2.0-line ducklake works: a manager is created **per transaction** (`DuckLakeTransaction`'s
constructor calls `DuckLakeMetadataManager::Create`, the registry by `MetadataType()`), and the
factory does not take a version — rightly, the version belongs to the catalog. Version-dependent
behaviour (the inlined-table column prefix, the exactness columns, the read queries) is driven from
`catalog.SupportsV1_1Metadata()` at 28 sites, never from the manager's class. The class matters for
exactly **seven virtuals**: the six catalog DDL statements and `GetVersionString()` (the base says
`1.0`), used only when a catalog is created.

The obstacle, and its exact boundary: `SetVersionedMetadataManager` runs at create (before
`InitializeDuckLake`) and at load (after the migration), and for a class it does not know it
**replaces the registered manager with a stock one** — at format 1.1 only (`if (version == V1_0)
return;` is its first line), and **only in the attach transaction**: every later transaction gets
our manager from the registry again. And the migrations (`MigrateV10`, `MigrateV10Dev`) are called
on our manager *before* the swap, so D1 is unaffected. What the swap costs us is precisely what our
manager does during the attach itself — the collation probe and the shaping in `InitializeDuckLake`
and `ProbeServerCapabilities`. (Discarding what the registry supplied is upstream's bug whether or
not we exist — ducklake#1066's libSQL manager meets the same wall — and the fix is one line: leave a
manager alone whose `GetVersionString()` already equals the requested version. Worth proposing,
without urgency.)

**So the manager stops depending on surviving the attach.** Everything it did at attach moves to the
**first use** in a transaction that is ours, guarded by the stamp: one `mssql_scan_unsafe` of the
shape stamp per process per catalog (today's attach reads it twice), and when the stamp is behind,
`EnsureCatalogShape` through `RunServerSideOutsideTransaction`, exactly as now. The shaping was
written for a catalog created by someone else's DDL — `NVARCHAR`, no keys, `key` unquoted — so a
catalog the stock manager created is just the case it already handles. Then:

- **1.0**: nothing changes; the swap never runs.
- **1.1, create**: the stock manager creates the catalog with stock DDL through duckdb (the generic
  manager could always create a catalog on SQL Server — specs/002, just keyless); our shaping brings
  it to shape on first use. The collation probe moves with it.
- **1.1, load**: the migration is ours (before the swap); the shaping, on first use.
- the seven DDL virtuals are not needed, and **the submodule patch goes** — the hook and the
  header-only template.

What it costs: the same work at a different moment — the first statement after an attach instead of
the attach, which to a user reads as "the first SELECT was slow", and DDL issued from a read, which
is unusual but no more so than from ATTACH. Races between processes are what they are today: the
shaping is idempotent. Per-process state: a flag per catalog so the stamp is read once, not per
transaction.

What has to be verified, and it is one run: that the 2.0-line stock `InitializeDuckLake` creates a
catalog through the 2.0-line mssql extension (its DML now goes through specs/080's writer). The
integration suite answers that the moment the patch is dropped and the shaping moved. If it does not
— the fallback is to pin new catalogs to 1.0 (`ducklake_default_version` defaulted by the extension,
`MigrateV10` refusing) and keep everything else of this design.

The strict guard (D4) stays as on the branch: it speaks at `DUCKLAKE_LATEST_VERSION`, and a 1.0
catalog takes the base path, correct and slower. `migrate_v11.test` stays as written — the upgrade
path is supported, not refused.

## Enforcement & security

- A migration step that fails surfaces as an error; nothing is swallowed into a warning. The rename
  is all-or-nothing.
- A declared scan shape that the stream does not match is an execution error naming the statement,
  never coerced.
- A bound the merge cannot order exactly is NULL, never a guess — and D6's table is what makes "cannot
  order" rare rather than common.
- The test runner echoes a failing statement **after** substitution: the DSN, password included,
  lands in a failed run's output. Redact before pasting a failure anywhere.

## Testing

- `test/sql/integration/migrate_v11.test` (71 assertions): a 1.0 catalog with inlined and file-backed
  data, attached plain (stays at 1.0), then with `AUTOMATIC_MIGRATION TRUE`, then twice more — the
  dev-format re-run and the explicit path on an already-migrated catalog.
- `test/sql/integration/given_shape.test` (24): the type rules, both refusals, the three composed
  shapes inside a transaction.
- `attach_mssql.test`: the partitioned column's stored bounds are 0 and 49 on both paths; a HUGEINT
  past the cast's range still answers a filter; the metadata log shows `mssql_scan_unsafe(`.
- `write_shapes.test`: `ducklake_table_info` answers 0 after a cascade because the eight table rows
  are ENDED, not deleted — not a format difference and not the batch's doing (`GetTableSizes` selects
  at the current snapshot; reproduced on a local-file lake at 1.0 and 1.1 alike).
- Both suites, default and fast path: **421 assertions each**, identical.
- `scripts/bench/attach_probe.py`: the per-statement attach, `--synth-tables N` for the one shape the
  rewriter is slow on (N distinct remote scans under `UNION ALL`: 0.63 s on / 0.016 s off at 1000 on
  the fixed mssql; the dry run's per-node CPU is on their list), `--shape-ab`, `--legacy-migration`.

## Alternatives considered

- **Patching DuckLake's migration SQL** for this backend — rejected: the embedded sources are never
  edited, and the virtuals exist for exactly this.
- **A dual-format commit batch** (kinds for 1.0 and 1.1) — rejected: a 1.0 catalog takes the base
  path correctly, and the strict guard's scope (D4) is cheaper and gives the re-audit trigger for free.
- **`remote_pushdown: false` for the metadata catalog** (design/004 §7's recommendation) — withdrawn:
  with D1 and D3 the attach no longer carries the statement the rewriter was slow on, and the arms are
  within noise.
- **Shadow typed columns** for the table-level bounds (our own `min_num`, `max_num`) — rejected:
  DuckLake's positional INSERTs on the base path would meet columns they do not know.
- **Merging the table-level bounds client-side** — rejected: it is a round trip, which is what phase 2
  exists to remove.

## Follow-ups

- D9: the shaping and the collation probe on first use behind the stamp, a per-process flag per
  catalog, the hook and the template dropped from the submodule, the suite run on untouched ducklake
  (the create-at-1.1 verification); the one-line upstream issue, without urgency.
- D6's exact comparison per type, and a decision on HUGEINT's top of range.
- D7 steps 1–4, in order; measure after each.
- The file-column-stats CTE of specs/008 as a direct scan (D5 makes it possible).
- Build `postgres_scanner` from source on the 2.0 line, so the postgres arm of the bench exists.
- `MSSQL_MEMORY_LIMIT_MB` in `docker/docker-compose.yml` (2 GB; `error 701` on the 1.1 catalog).
- design/005 §9's watchlist at the bump: ducklake#1066 (a libSQL manager proposed in-tree), Turso 0.8.
