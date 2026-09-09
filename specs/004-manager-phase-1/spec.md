# Spec 004: the SQL Server metadata manager, phase 1 — parity with postgres

- **Status**: accepted (in progress)
- **Date**: 2026-09-09
- **Author**: VGSML
- **Depends on**: [002](../002-embedded-ducklake/spec.md) (the embedded extension), [003](../003-mssql-extension-v0.2.5/spec.md) (mssql v0.2.5)
- **Followed by**: 005 (phase 2 — the server-side `ducklake_commit` procedure)

## Summary

`MSSQLMetadataManager` stops inheriting DuckLake's generic behavior in the three places the postgres
manager overrides, plus the two SQL Server needs of its own. Writes stop going through DuckDB's DML
path — the commit batch is handed to `mssql_exec` as raw T-SQL — the catalog is created by our own
DDL with primary keys and filtered indexes, and the columns DuckLake inlines get SQL Server types we
choose. This is the phase that makes the lake writable; phase 2 makes it fast.

## Problem

Through the generic manager a table's **first** write commits and every later one fails (verified
2026-09-08, mssql v0.2.5, pinned as a `statement error` in `test/sql/integration/attach_mssql.test`):

```
Failed to flush changes into DuckLake: MSSQL: UPDATE/DELETE requires a table with a primary key.
Table 'dbo.ducklake_table_stats' has no primary key.
```

Once a table has statistics, the commit batch UPDATEs `ducklake_table_stats`; the generic manager
runs the batch as DuckDB SQL against the attached catalog, so that UPDATE takes DuckDB's DML path,
and the mssql extension needs a primary key to build a rowid for it. A DELETE of an inlined row
fails the same way on `ducklake_inlined_data_<table>_<version>`.

Two more things are wrong before anything is measured. Every metadata statement is its own round
trip, where postgres sends the batch once. And the catalog is created by DuckDB's `CREATE TABLE`,
which gives it no keys and no indexes — while almost every DuckLake read carries
`WHERE end_snapshot IS NULL`.

## Design

### D1 — `Execute` is a passthrough (the postgres precedent)

`Execute(snapshot, query)` substitutes the placeholders itself, transpiles the batch, and sends it
as one statement:

```sql
SELECT mssql_exec('<metadata catalog name>', '<the transpiled batch>')
```

Placeholder substitution copies `PostgresMetadataManager::ExecuteQuery` including its one
divergence from the base: `{METADATA_CATALOG}` expands to the **schema identifier alone**, not
`catalog.schema` — the SQL now runs inside SQL Server, where the catalog name is not a prefix.
Reads keep the base `Query()` (DuckDB scans of the attached catalog, materialized by mssql v0.2.5
when a plan holds several of them).

Consequence: DuckDB's DML path is never involved in a lake write, so the primary-key requirement
does not apply — the failure above disappears, and with it the retry storm behind it.

### D2 — write our own T-SQL where a virtual exists; rewrite the rest, literal-aware

The obvious design is to generate T-SQL ourselves and never rewrite a string. DuckLake does not
allow it: **most of the batch generators are `static`, not virtual** — `WriteNewSchemas`,
`WriteNewTables`, `WriteNewViews`, `WriteNewColumns`, `WriteNewTags`, `WriteNewPartitionKeys`,
`DropTables`, `WriteNewDataFilesSqlBatch`, `InsertSnapshotSql` — and `DuckLakeTransactionState`
calls them directly. Editing the submodule is out (CLAUDE.md's standing invariant). So for that
majority the assembled batch text reaching `Execute` is the only seam there is.

The split is therefore:

- **Ours, generated directly as T-SQL** — every place DuckLake left a virtual and the dialect or
  the types matter: `GetInlinedTableQueries` (the inlined table's DDL, hence D4's collation and
  types), `WriteNewInlinedData`, `WriteNewInlinedTables`, `WriteNewInlinedFileDeletes`,
  `WriteNewDataFiles`, and `InitializeDuckLake` (D3). No rewriting is involved in any of them.
- **Rewritten in `Execute`** — what the static generators produced. A closed list of five forms,
  applied by a scanner that walks the batch and distinguishes code from data: it steps over `'...'`
  literals (including `''` escapes) and quoted identifiers, and substitutes only in code positions.
  This is the part a regex would get wrong — a user string containing `NOW()` or the word `true` is
  data and must survive untouched — and it is why the scanner is worth its ~60 lines. An
  unrecognized dialect marker throws rather than reaching the server.

The forms, from an audit of `ducklake_metadata_manager.cpp` at the pinned submodule:

| DuckDB form | T-SQL |
| --- | --- |
| `NOW()` | `SYSDATETIMEOFFSET()` |
| `true` / `false` as values | `1` / `0` |
| `CREATE TABLE IF NOT EXISTS x(...)` | `IF OBJECT_ID('x') IS NULL CREATE TABLE x(...)` |
| `WITH cte(a, b) AS (VALUES ...)` | `WITH cte(a, b) AS (SELECT * FROM (VALUES ...) v(a, b))` |
| identifiers colliding with T-SQL reserved words (`key`) | quoted |

`DROP TABLE IF EXISTS` and `UPDATE ... SET ... FROM ...` are valid T-SQL as generated. Statements
that only appear in DuckLake's own migrations (`UPDATE t AS alias`, `LIST(...)`) are out of scope:
`automatic_migration` is off and the metadata version is pinned.

The list is small and lives in one place on purpose. A ducklake submodule bump re-runs the audit and
the full smoke; that is the price of vendoring, and it is written down in CLAUDE.md. Every form the
audit finds is also covered by a unit-level test over the scanner, including the cases where the
same text appears inside a string literal and must not change.

### D3 — our own `InitializeDuckLake`

Instead of DuckLake's DuckDB DDL:

- **Primary keys** on the catalog tables, so a future non-passthrough path is not blocked and the
  server can enforce what DuckLake assumes.
- **Filtered indexes** `WHERE end_snapshot IS NULL` on the versioned tables — the condition almost
  every read carries. Neither postgres nor sqlite has indexes here; this is the first place we can
  be faster rather than equal.
- **A server gate**: SQL Server 2019 or newer, checked with
  `CAST(SERVERPROPERTY('ProductMajorVersion') AS INT)` (uncast it returns `sql_variant`, which the
  mssql extension cannot decode — it tears the connection). Older servers have no UTF-8 collation
  and are refused with a message naming the reason.
- **Explicit `COLLATE` on every VARCHAR column** (D4), never the database default: DuckLake pushes
  string comparisons against `min_value`/`max_value` computed by DuckDB in UTF-8 byte order, so the
  column has to compare binary. A UTF-8 `_BIN2_` collation is exactly that; a CI/AS one prunes
  wrongly and silently drops rows. A warning if the database's own collation is not UTF-8, because
  everything the user creates in that database is then not what this catalog assumes.

### D4 — the inlining type matrix

`TypeIsNativelySupported` and `GetColumnTypeInternal` per the research note's matrix: BOOLEAN→BIT,
UTINYINT→TINYINT, TINYINT→SMALLINT, DECIMAL(p≤38), BLOB→VARBINARY(MAX), DATE, TIME(6),
DATETIME2(6), DATETIMEOFFSET, UUID→UNIQUEIDENTIFIER. Not native, so stored as text and cast back by
`TransformInlinedData`: FLOAT/DOUBLE (SQL Server has no NaN/±Inf), TIMESTAMP_NS (DATETIME2(7) is
100 ns), HUGEINT/UHUGEINT (wider than DECIMAL(38,0)), INTERVAL, and everything nested.

Strings — the inlined user columns and the text fallbacks — are
`VARCHAR(MAX) COLLATE Latin1_General_100_BIN2_UTF8`: the server stores the UTF-8 bytes DuckDB
already has, so a read is a copy rather than a UTF-16 transcode, and the ordering matches DuckDB's.
`MSSQL_VARCHAR(n)` cannot say MAX (mssql-extension#321), which is why the DDL is ours rather than a
cast on a CTAS.

### D5 — the hot reads through `mssql_scan`

`GetLatestSnapshotQuery` runs on every transaction start; it becomes a single
`mssql_scan('<catalog>', '...')`. `GenerateFileColumnStatsCTEBody` stays a catalog scan for now: its
CTE lives inside a query that also joins catalog tables, and on mssql v0.2.5 an `mssql_scan()` may
only be the sole source of its query (spec 003). Revisit when that constraint lifts on the 2.0 line.

## Enforcement & security

Fail-closed: an old server is refused at initialization rather than silently given a lossy catalog,
and the transpiler throws on a form it does not recognize rather than sending SQL it has not
checked. The passthrough sends SQL DuckLake generated and we transpiled — no user string reaches it
unquoted that would not have reached the generic path the same way.

## Testing

- The `statement error` in `test/sql/integration/attach_mssql.test` becomes real assertions: the
  second insert, UPDATE, DELETE (inlined and file-backed), and a commit past the inlining limit.
- A full cycle with inlining at the default limit of 10 rows, including the awkward types of D4:
  FLOAT NaN, TIMESTAMP_NS, HUGEINT, STRUCT, and a non-ASCII string round trip.
- The catalog's shape after `InitializeDuckLake`: keys and filtered indexes present, VARCHAR
  columns carrying the collation.
- The server gate's message on a pre-2019 server is not testable in CI (the image is 2025); the
  version probe itself is asserted instead.

## Alternatives considered

- **Generate every statement ourselves and never rewrite a string.** Not available: the static
  generators above have no seam, and patching the submodule is forbidden. What is reachable through
  a virtual, we do generate ourselves (D2).
- **Keep the generic manager and give the catalog primary keys only.** Enough to stop the observed
  failure, but it leaves every statement its own round trip and hands DuckDB's DML path a rowid it
  would have to build per row.
- **Route every read through `mssql_scan` too.** That is the phase-3 transpiler over ~25 read
  queries with list aggregation and `NULLS FIRST`; correctness does not need it (v0.2.5 materializes
  multi-scan plans) and it would be re-audited on every bump.
- **Wait for phase 2 and do both at once.** The server-side procedure duplicates part of the commit
  protocol in T-SQL; it wants a working, measured phase 1 underneath it.

## Follow-ups

- Spec 005: the `ducklake_commit` procedure, `ProbeServerCapabilities`, `CanSkipSnapshotFetch`,
  `FlushChangesServerSide` — data-only commits in one round trip with server-side retry.
- The BCP staging path for large inlined writes (research note §7): `##stage` filled by
  `COPY … (FORMAT 'bcp')` on a second connection, moved into place by the commit batch.
- Bench against the postgres backend on the same dataset; the target is "not worse", and the
  filtered indexes are where it should be better.
- `docker/init/sqlserver.sql` creates `lake_meta` with the database default collation, which on the
  image is `SQL_Latin1_General_CP1_CI_AS` — create it UTF-8 so the tests exercise what the README
  recommends.
