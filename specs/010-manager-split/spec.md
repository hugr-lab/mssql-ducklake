# Spec 010: splitting the manager, and two documents that say the wrong thing

- **Status**: draft
- **Date**: 2026-09-10
- **Author**: VGSML

## Summary

`mssql_metadata_manager.cpp` is 1132 lines in five sections, and two of its functions — the DDL
that shapes the catalog — are 37% of it. specs/008 and 009 add a third kind of code, the pure-T-SQL
layer. This spec cuts the file along the sections it already has, with **no behavioural change**,
and fixes two claims in the project's own documents that the code no longer supports.

## Problem

Three kinds of code share one file and one header:

| section | lines | what |
| --- | ---: | --- |
| Initialization (specs/004 D3) | ~290 | `InitializeDuckLake` 199, `EnsureCatalogShape` 223, `CatalogShapeIsCurrent`, the key/index/collation tables, the shape version |
| Phase 2 (specs/005) | ~470 | `StageCommitLocally`, `StageCommit`, `FlushChangesServerSide`, `IsDataFilesOnlyCommit`, three switches |
| queries in T-SQL (specs/005 D13, 007) | ~150 | `GetLatestSnapshotQuery`, the conflict-check constants and `Query` override |
| type matrix, server plumbing, inlined DDL (specs/004) | the rest | |

A reader looking for how a commit is staged scrolls past 200 lines of `ALTER TABLE`; a reader
auditing what SQL the manager sends the server — the question every ducklake bump asks — has to
find it in three places. specs/008 adds five more query overrides to the third kind.

Two documents are wrong about the code:

- **CLAUDE.md** says, in three places, that "Execute-passthrough kills the PK problem" — every
  write "flows through one `Execute` seam". The manager does not override `Execute` at all. The
  commit batch runs through DuckDB's DML path against a catalog the manager keyed for exactly that
  purpose (specs/004 D3); the header of `mssql_metadata_manager.hpp` says so, correctly. The
  wording is a leftover of design 001 §4, which was superseded by the keys before it was built.
- **CLAUDE.md** says a plan mixing `mssql_scan()` with a catalog scan inside a transaction "still
  fails (verified)". Measured on 20,000-row results (design 002 §5), it fails **depending on the
  execution order** — `catalog JOIN mssql_scan` passes when the plan happens to drain the stream
  before the second source needs the connection, and the same pair fails as a CTE. The rule the
  manager follows ("`mssql_scan` only as the sole source of its query") is right either way; the
  sentence should not invite someone to disprove it with one lucky plan.

And four environment switches — `MSSQL_DUCKLAKE_SERVER_COMMIT`, `_SERVER_COMMIT_MIN_FILES`,
`_SERVER_COMMIT_SKIP_FETCH`, `_NO_CONFLICT_REWRITE` — are defined where each was needed, three in
one pattern and one in another.

## Design

Four files, one class, cut along the existing `//===` sections:

| file | holds |
| --- | --- |
| `src/mssql_catalog_shape.cpp` | `InitializeDuckLake`, `EnsureCatalogShape`, `CatalogShapeIsCurrent`, the `keys` / `visibility_indexes` / `live_indexes` / `stats_columns` tables, `SHAPE_VERSION` handling |
| `src/mssql_server_commit.cpp` | phase 2 end to end: `IsDataFilesOnlyCommit`, `StageCommitLocally`, `StageCommit`, `CanSkipSnapshotFetch`, `FlushChangesServerSide`, the apply's T-SQL |
| `src/mssql_metadata_queries.cpp` | the T-SQL layer: `GetLatestSnapshotQuery`, the 007 constants and `Query(DuckLakeSnapshot, string &)`, the specs/008 overrides |
| `src/mssql_metadata_manager.cpp` | constructor, the type matrix, `RunOn` / `RunServerSide` / `RunServerSideOutsideTransaction`, `ClearCache` / `InvalidateTableCache`, `GetInlinedTableQueries` / `GetInlinedDeletionTableName`, `ProbeServerCapabilities` |

The header stays one `MSSQLMetadataManager`; the split is of definitions, not of the class. Private
helpers that cross files (`SchemaIdentifier`, `CatalogLiteral`, `TSQLLiteral`) move to the header
or to a small `mssql_metadata_internal.hpp`. `CMakeLists.txt` lists the new sources.

The switches move into one block at the top of `mssql_server_commit.cpp` (three of them) and
`mssql_metadata_queries.cpp` (the fourth), each as the `static const bool` read-once form the first
three already use, with one comment explaining that a getenv per call would be a syscall on the
hot path.

CLAUDE.md:

- the "Execute-passthrough" bullet is rewritten to what the code does: the catalog is keyed so
  DuckLake's own SQL runs through DuckDB's DML path; the appender is the one write that takes that
  path by design (specs/006 D1); nothing passes through an `Execute` override because there is
  none;
- the `mssql_scan()` bullet says "fails depending on execution order", cites the measurement, and
  keeps the rule.

Nothing else moves. Comments stay with the code they describe; the git history of each block is
preserved by moving whole functions rather than rewriting them (`git log --follow` will not track
across a split, so the commit message names the source section of every block).

## Enforcement & security

No behavioural change. Every switch keeps its name and default.

## Testing

Identity, not new coverage: every suite passes with the same assertion counts before and after —
unit (17), integration (163) on both commit paths, `make test-concurrent`, smoke load, format and
tidy checks. The distribution build runs on the merge to `main`.

## Alternatives considered

- **Split the class too** — a `CatalogShaper`, a `ServerCommit`. More seams than the code has
  reasons for; the manager is one object with one transaction, and the base class's virtuals all
  land on it.
- **Leave it until after the release.** The release tag should stand on the layout the next work
  will live in; and specs/008 would otherwise add its overrides to a file already too long to
  review in one sitting.

## Follow-ups

- After the 2.0 bump the T-SQL layer grows again (`GenerateFileColumnStatsCTEBody`, design 002
  §5); it has its own file by then.
