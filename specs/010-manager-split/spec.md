# Spec 010: splitting the manager, and two documents that say the wrong thing

- **Status**: implemented
- **Date**: 2026-09-10
- **Author**: VGSML

## Summary

`mssql_metadata_manager.cpp` had grown to 1260 lines holding four kinds of code, and two functions
— the DDL that shapes the catalog — were a quarter of it. This spec cuts the file along the
sections it already had, with **no behavioural change**, and fixes the claims in CLAUDE.md that the
code never supported. Two things it measured on the way in (specs/009 and 012) made the second half
more than a wording fix: the "Execute-passthrough" the document described is exactly the commit
path the benchmark shows is not there.

## Problem

Four kinds of code shared one file and one header:

| section | lines | what |
| --- | ---: | --- |
| Initialization (specs/004 D3, 006, 012) | ~340 | `InitializeDuckLake`, `EnsureCatalogShape`, `ApplyForcedParameterization`, `CatalogShapeIsCurrent`, the key/index/collation tables, the shape version |
| Phase 2 (specs/005) | ~370 | `CommitBatchSql`, `StageCommitLocally`, `StageCommit`, `CanSkipSnapshotFetch`, `FlushChangesServerSide`, `IsDataFilesOnlyCommit`, three switches |
| queries in T-SQL (specs/005 D13, 007, 008) | ~250 | the conflict-check constants and `Query` override, `GetInlinedDeletionTableName`, `GetLatestSnapshotQuery` |
| the manager (specs/004) | ~280 | constructor, the type matrix, `RunOn` and friends, `ClearCache`, `GetInlinedTableQueries`, `ProbeServerCapabilities` |

A reader looking for how a commit is staged scrolled past 230 lines of `ALTER TABLE`; a reader
auditing what SQL the manager sends the server — the question every ducklake bump asks — found it
in three places. The four environment switches were defined where each was needed, three in one
pattern and one in another, and `TSQLLiteral` sat in the shaping section while the commit batch
used it too.

Two documents were wrong about the code:

- **CLAUDE.md** said, in three places, that "Execute-passthrough kills the PK problem" — every
  write "flows through one `Execute` seam". The manager does not override `Execute` at all; specs/004
  decided that ("no transpiler"), and specs/009's `MSSQL_DEBUG=2` stream shows what it means: the
  commit batch runs statement by statement through the mssql extension's DML operators, ~19 round
  trips, with a full scan of `ducklake_table_column_stats` for its stats UPDATE — against a catalog
  the manager keyed for exactly that purpose (specs/004 D3). The header of
  `mssql_metadata_manager.hpp` said so, correctly. The wording was a leftover of design 001 §4,
  superseded by the keys before it was built. A comment in `CanSkipSnapshotFetch` repeated it.
- **CLAUDE.md** said a plan mixing `mssql_scan()` with a catalog scan inside a transaction "still
  fails (verified)". Measured on 20,000-row results (design 002 §5), it fails **depending on the
  execution order** — `catalog JOIN mssql_scan` passes when the plan happens to drain the stream
  before the second source needs the connection, and the same pair fails as a CTE. The rule the
  manager follows ("`mssql_scan` only as the sole source of its query") is right either way; the
  sentence should not invite someone to disprove it with one lucky plan.

## Design

Four source files and one internal header, one class, cut along the existing `//===` sections:

| file | lines | holds |
| --- | ---: | --- |
| `src/mssql_metadata_manager.cpp` | 283 | constructor, the type matrix, `RunOn` / `RunServerSide` / `RunServerSideOutsideTransaction`, `ClearCache` / `InvalidateTableCache`, `ProbeServerCapabilities`, `GetInlinedTableQueries` |
| `src/mssql_catalog_shape.cpp` | 352 | `CatalogShapeIsCurrent`, `EnsureCatalogShape`, `ApplyForcedParameterization`, `InitializeDuckLake` |
| `src/mssql_server_commit.cpp` | 386 | phase 2 end to end: `CommitBatchSql`, `StageCommitLocally`, `StageCommit`, `IsDataOnlyCommit` / `IsDataFilesOnlyCommit`, `CanSkipSnapshotFetch`, `FlushChangesServerSide` |
| `src/mssql_metadata_queries.cpp` | 257 | the two conflict-check constants and `Query(DuckLakeSnapshot, string &)` (specs/007), `ServerScan` / `ScalarOf`, `GetInlinedDeletionTableName`, `GetLatestSnapshotQuery` (specs/008) |
| `src/include/mssql_metadata_internal.hpp` | 65 | the four switches, `TSQLLiteral`, and the guard `ConflictCheckQueryIsDuckLakes()` |

The class header stays one `MSSQLMetadataManager`; the split is of definitions, not of the class,
and its opening comment names the four files. What crosses files goes to the internal header,
header-only on purpose: `TSQLLiteral` (shaping and the commit batch) and the four switches are
`inline` functions — an inline function's local `static` is one instance per program, so each
switch is still read once, in the form the first three already used — and the one thing that
could not be inlined without dragging DuckLake's 50-line query text into a header is the guard:
`ProbeServerCapabilities` (manager file) used to compare the query against the constant that lives
with the rewrite (queries file); it now asks `ConflictCheckQueryIsDuckLakes()`, defined next to the
constant. That and the trailing newline the join dropped are the only edits to moved code; every
block moved whole, comments with it. `CMakeLists.txt` lists the four sources.

CLAUDE.md:

- the "Execute-passthrough" bullet is rewritten to what the code does: the catalog is keyed so
  DuckLake's own SQL runs through DuckDB's DML path, statement by statement, the appender the same
  way (specs/006); nothing passes through an `Execute` override because there is none; the round
  trips that costs, and that the server-side commit is the open item. The second mention, in the
  "where the generic manager stops" bullet, says "primary keys" instead;
- the `mssql_scan()` bullet says "fails depending on execution order", cites the measurement, and
  keeps the rule;
- the project-structure tree lists the four files and the internal header.

Nothing else moves. `git log --follow` does not track across a split, so the commit names the
source section of every block.

## Enforcement & security

No behavioural change. Every switch keeps its name and default.

## Testing

Identity, not new coverage — every suite passes with the same assertion counts before and after,
on the same build machine: unit 17 (2 cases, 1 skipped), integration **189** on both commit paths
(the switch off and `MSSQL_DUCKLAKE_SERVER_COMMIT=1`), `make test-concurrent` (all writers
committed everything), the smoke load (both gates), format-check, tidy-check (0 warnings). The
distribution build runs on the merge to `main`.

## Alternatives considered

- **Split the class too** — a `CatalogShaper`, a `ServerCommit`. More seams than the code has
  reasons for; the manager is one object with one transaction, and the base class's virtuals all
  land on it.
- **Leave it until after the release.** The release tag should stand on the layout the next work
  will live in; and the server-side commit spec would otherwise add to a file already too long to
  review in one sitting.
- **A nested namespace or prefixed names for the shared helpers**, against symbol collisions in the
  one image that also embeds ducklake: the helpers are `inline` and hidden-visibility, and the one
  non-inline function has a name nothing else would choose; not worth the qualification at every
  call site.

## Follow-ups

- After the 2.0 bump the T-SQL layer grows again (`GenerateFileColumnStatsCTEBody`, design 002
  §5); it has its own file now.
- The server-side commit (specs/009's finding, design 001 §7) lands in `mssql_server_commit.cpp`,
  which is what that file is for.
