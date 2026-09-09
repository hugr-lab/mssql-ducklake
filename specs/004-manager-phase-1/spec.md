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

### D1 — the statement matrix: what runs where, and why

The first cut of this spec sent the whole commit batch through `mssql_exec` and rewrote DuckLake's
SQL into T-SQL on the way. That worked — the cycle went green — but the price was six rewrite rules,
three of which touched **values**, and one of those shipped a silent data bug: a bare `'text'`
literal is parsed in the database's collation code page, so `привет 😀` reached a UTF-8 column as
`?????? ??` with no error anywhere. Rewriting SQL we did not generate is not a foundation.

So the question is not "how do we transpile" but "which statements actually need us". Taking the
inventory rather than guessing, over the pinned submodule:

| statement, and where it comes from | runs through | why |
| --- | --- | --- |
| `INSERT INTO ducklake_*` — the bulk of every commit: inlined rows, data files, stats, tags, snapshot changes | **duckdb** | DuckDB already writes into an attached mssql catalog correctly — values, types, quoting, Unicode. Nothing to rewrite, so nothing to get wrong. |
| `UPDATE t SET end_snapshot = N WHERE end_snapshot IS NULL AND id IN (…)` — dropping schemas, tables, views, macros | **duckdb** | valid DuckDB SQL; the only thing that ever blocked it was the missing primary key (D3) |
| `WITH cte(…) AS (VALUES …) UPDATE t SET … FROM cte` — table and column statistics, tags, dropped columns, inlined deletes | **duckdb** | same: DuckDB parses and plans it, and with a key on the target the mssql DML path can execute it |
| `DELETE FROM t WHERE …` — expiry and cleanup | **duckdb** | same |
| `CREATE TABLE IF NOT EXISTS ducklake_inlined_data_*` (`GetInlinedTableQueries`, virtual) and `…_inlined_delete_*` (`GetInlinedDeletionTableName`, virtual) | **ours, raw T-SQL** | we choose the column types and the collation (D4); DuckDB cannot express `VARCHAR(MAX) COLLATE …`, and letting it create the table would hand the choice to the mssql extension's global settings |
| the catalog's own DDL (`InitializeDuckLake`, virtual) | **ours, raw T-SQL** | primary keys and filtered indexes, which DuckLake's DDL has none of (D3) |
| reads: catalog load, file lists, snapshots, statistics | **duckdb scans** | correct as they are; mssql v0.2.5 materializes a plan that holds several of them on the pinned connection (spec 003) |
| `GetLatestSnapshotQuery` — every transaction start | **`mssql_scan`** | the one hot read: one server-side statement instead of a scan of the whole table (D5) |

Two things fall out of the matrix, and both are the point of it:

- **There is no transpiler.** Not a smaller one — none. Every statement DuckLake generates runs as
  DuckLake wrote it, and every statement in T-SQL is one we wrote ourselves.
- **`Execute` is not overridden.** The base implementation is the duckdb path, which is what the
  matrix asks for.

### D2 — the DDL is written, not rewritten

Both DDL sites are virtual, so the T-SQL is ours to emit directly. They are also the only statements
in the batch that DuckDB could not carry, which is why they are the only ones that leave it.

They are executed through `mssql_exec` at the moment the virtual is called, rather than returned into
the batch: the batch is DuckDB SQL and a T-SQL statement inside it would not parse. Both are
`IF OBJECT_ID(…) IS NULL`-shaped, so a commit retry — which regenerates the batch and calls the
virtual again — is a no-op the second time.

Two things about *how* they run were found the hard way, and both are load-bearing:

- **On a connection of their own, in autocommit.** Run inside the transaction, the new table is
  locked against the metadata query the mssql extension issues to discover it — and that query takes
  its own connection, so it waits on us until it times out. Created in autocommit the table is
  visible at once and holds nothing. A rolled-back commit then leaves an empty table behind, which
  the next attempt reuses; DuckLake's own DDL is `IF NOT EXISTS`-shaped for the same reason.
- **The cache is dropped after the commit, not after the DDL.** The extension caches catalog
  metadata, and a table created behind its back is invisible to the reads that follow — they miss it
  silently rather than failing. DuckLake already knows when that happened and calls `ClearCache()`
  at the right moment, so that is where `mssql_invalidate_cache` goes. Called mid-transaction it
  deadlocks the same way as the DDL did. The extension's own `mssql_exec_invalidate_cache` setting
  does this globally and on every DML; this is the point version.

The inlined data table also gets a primary key, `(row_id, begin_snapshot)`, for the same reason the
catalog's tables do (D3): its rows are updated and deleted.

### D3 — our own `InitializeDuckLake`

DuckLake's own DDL runs first, through DuckDB — it owns the shape of its catalog, and reproducing
twenty-eight `CREATE TABLE`s here would be a copy to re-audit on every submodule bump. What follows
is the part DuckDB cannot express, applied as our T-SQL:

- **Primary keys** on the catalog tables, so a future non-passthrough path is not blocked and the
  server can enforce what DuckLake assumes.
- **Filtered indexes** `WHERE end_snapshot IS NULL` on the versioned tables — the condition almost
  every read carries. Neither postgres nor sqlite has indexes here; this is the first place we can
  be faster rather than equal.
- The keys and the column changes go in **two batches**: inside one T-SQL batch a column's new
  `NOT NULL` is not yet visible to the constraint that needs it, and the server answers "cannot
  define PRIMARY KEY on a nullable column".
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

- **Pass the whole batch through `mssql_exec`, rewriting it into T-SQL** (this spec's first cut,
  implemented and measured). One round trip per commit instead of several, and the postgres manager
  does exactly this — but postgres transpiles nothing, because DuckLake's SQL already is postgres's
  dialect. For us it meant six rewrite rules over SQL we did not generate, three of them touching
  values; one silently mangled every non-ASCII string. Reverted in favour of the matrix. The
  round-trip count comes back in phase 2, where the server-side procedure earns it honestly.
- **Let DuckDB create the inlined tables too**, and take whatever types the mssql extension maps.
  Then the collation — which decides whether DuckLake's pushed-down `min`/`max` comparisons prune
  correctly — would follow the extension's global settings rather than this manager's choice.
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
