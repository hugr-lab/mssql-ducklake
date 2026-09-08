# Spec 003: mssql-extension v0.2.5 — what the DuckLake manager needs from the runtime pair

- **Status**: implemented — shipped as mssql `v0.2.5` (2026-09-08, from the `duckdb-v1.5.5`
  branch; community-extensions PR duckdb/community-extensions#2676); this repository's pin is bumped
  to it. R1 and R2 landed as specified (mssql-extension #313, #318). **R3 did not**: nothing under
  `src/tds/` changed between the tags, and the per-catalog mutex that shipped is taken only on the
  R2 materialization path (`table_scan.cpp`, `if (bind_data.requires_materialization)`), so it
  serializes that path rather than making an unmaterialized second batch fail cleanly — every shape
  R3 targeted still tears the stream. Verified live: through the generic manager a table's first
  write commits and its second fails; two caveats, both recorded below, shape spec 004.
- **Date**: 2026-09-08
- **Author**: VGSML
- **Depends on**: [002](../002-embedded-ducklake/spec.md) (the live-attach findings)
- **Unblocks**: 004 (manager phase 1)

## Summary

The first live `ATTACH 'ducklake:mssql:…'` (spec 002, research note §9) showed that the embedded
ducklake already initializes and re-opens a catalog in SQL Server through the generic manager, and
that two behaviors of `mssql` v0.2.4 stop everything after that. Both belong in the mssql extension,
not in the manager: (1) the catalog reports duckdb's `main` as its default schema, and (2) inside an
explicit transaction, a query with two scans of the same catalog breaks on the one connection pinned
to that transaction. This spec is the brief for a session in the mssql-extension repository: the
exact change, the reference implementation it mirrors, where it goes on the v1.5.5 line, and how it
is proven. It is deliberately small — the mechanism is the one mssql-extension's own spec 066
(issue #239) already designed for the sink-driven case; what is missing is the read-only gate.

## Problem

### P1 — default schema is `main`

`DuckLakeTransaction::GetDefaultSchemaName()` (`ducklake/src/storage/ducklake_transaction.cpp:1537`)
asks the attached metadata catalog for `Catalog::GetDefaultSchema()`. `MSSQLCatalog` does not
override it, so duckdb's base answer `main` comes back and the attach fails with
`Schema 'main' not found in MSSQL database`. The workaround until v0.2.5 was `METADATA_SCHEMA 'dbo'`
on every ATTACH; mssql-extension issue #129 shows a user working around the same thing by creating a
schema called `main` in SQL Server. (v0.2.5 answers the constant `dbo`, not `SCHEMA_NAME()`, so a
login whose own default schema is elsewhere still passes `METADATA_SCHEMA`.)

### P2 — two scans of one catalog on the pinned connection

ducklake keeps its metadata connection in an explicit transaction
(`ducklake_transaction.cpp:790`), so every metadata read runs on the TDS connection pinned to that
transaction (`ConnectionProvider::GetConnection`, `src/connection/mssql_connection_provider.cpp:109`
at v0.2.4, "Explicit transaction mode" branch at `:140`). The catalog scan executes its batch and
reads COLMETADATA inside `TableScanInitGlobal` (`src/table_scan/table_scan.cpp:191`, the
`executor.Execute(context, query)` at `:450` → `MSSQLResultStream::Initialize`,
`src/query/mssql_query_executor.cpp:99`) and drains the rows lazily from `GetData`. DuckDB does not
promise that one source is drained before the next is initialized. For a decorrelated subquery
(`LEFT_DELIM_JOIN` + `DELIM_SCAN`) it initializes both, and the second `ExecuteBatch` finds the
connection in `Streaming` (`src/tds/tds_connection.cpp:1260`):

```text
IO Error: Failed to get partition information from DuckLake: Failed to execute SQL batch:
Cannot execute: connection not in Idle state (current: Executing)
```

With `threads > 1` the two sources run concurrently and the failure is a torn protocol instead:
`Connection closed while waiting for COLMETADATA`.

Measured on v0.2.4 against SQL Server 2025, all inside `BEGIN … COMMIT` on an attached catalog
holding the ducklake tables:

| query shape | result |
| --- | --- |
| `a JOIN b USING (k)` — hash join, with or without ORDER BY, threads 1 and 4 | passes |
| ducklake's tables query: LEFT JOIN + three correlated subqueries + ORDER BY | passes |
| ducklake's views query: one correlated subquery over `ducklake_tag` (plan: `LEFT_DELIM_JOIN`) | **fails**, also with ORDER BY |
| the same views query in autocommit (each scan takes a pool connection) | passes |
| the same rewritten as `LEFT JOIN … GROUP BY` | passes |

The failing query is `DuckLakeMetadataManager::GetCatalogForSnapshot`'s view load
(`ducklake_metadata_manager.cpp:746`; its error text says "partition information" — a ducklake
copy-paste), and it runs before every DDL/DML on the lake. The manager cannot avoid it: ducklake's
read path over the attached catalog has ~25 such queries, and re-routing all of them through
`mssql_scan` with a duckdb→T-SQL transpile of list aggregation, `::VARCHAR`, `NULLS FIRST` and
correlated subqueries is the manager's "phase 3" — large, and re-audited on every ducklake bump.
Separate connections for reads are not an option either: after the commit batch has executed on the
pinned connection, its uncommitted rows are locked, and a read from another connection blocks under
READ COMMITTED. So this is the runtime pair's problem, and it has a known solution.

## Design

The reference is duckdb-postgres, which has the same constraint (one connection per transaction,
one active COPY per connection). Its optimizer extension counts postgres scans per catalog in the
plan; when there is more than one and separate connections cannot be used, every such scan gets
`requires_materialization`, and `InitGlobal` drains the whole result into a `ColumnDataCollection`
before returning, with `MaxThreads() = 1` (duckdb-postgres at `fffcb35`, 2026-09-02, the copy
duckdb-acl's build vendors: `src/storage/postgres_optimizer.cpp:78-96`,
`src/postgres_scanner.cpp:298-306, 373-396`). mssql-extension's spec 066 (branch
`spec/065-dml-pushdown-recon`, `specs/066-own-scan-materialization/spec.md`) already adopts this
mechanism — D1: `requires_materialization` on the bind data, drain in `TableScanInitGlobal`,
`MaxThreads = 1`, a plan-walk helper — for the *sink* cases (INSERT/CTAS/UPDATE/DELETE reading
their own catalog). What v0.2.5 needs is D1 plus one more gate, and it needs it on the v1.5.5 line.

### R1 — `MSSQLCatalog::GetDefaultSchema()` returns the connection's default schema

Override in `src/include/catalog/mssql_catalog.hpp` (class at `:45`). `dbo` is the right constant
answer for v0.2.5; if cheap, resolve `SCHEMA_NAME()` once at attach and cache it (a login whose
default schema is not `dbo` then gets the truth). Effect: `USE db; SELECT current_schema()` gives
`dbo`, and ducklake stops needing `METADATA_SCHEMA`.

### R2 — spec 066 D1, backported, with a read-only gate

- **Mechanism (066 D1, unchanged)**: `requires_materialization` on the catalog-scan bind data; when
  set, `TableScanInitGlobal` runs the existing stream to completion into a `ColumnDataCollection`
  and releases the connection before returning; `MSSQLScanGlobalState::MaxThreads()`
  (`src/include/table_scan/table_scan_state.hpp:49`) returns 1; `TableScanExecute` serves chunks from
  the collection. The stream class is reused; the new code is the drain loop, ~40 lines as in
  postgres.
- **Gate (new, the read-only case)**: in `MSSQLOptimizer::Optimize`
  (`src/table_scan/mssql_optimizer.cpp:650`, registered at `src/mssql_extension.cpp:208`), gather
  the `LogicalGet`s whose function is the mssql catalog scan, grouped by catalog. For a catalog with
  **more than one** scan in the plan **and** a client context that is not in autocommit
  (`context.transaction.IsAutoCommit()` — the DML executors' test), flag them all. Autocommit stays
  exactly as today: single-scan plans stream, multi-scan plans stream on pool connections.
- **Why "all of them"**: which source DuckDB initializes first is a scheduling detail, not a plan
  property (the `LEFT_DELIM_JOIN` case above initializes both before draining either). Postgres
  materializes all of them in this situation too. Memory is bounded by the buffer manager
  (spillable collection) and, for the ducklake workload, by tiny metadata tables.
- 066's sink gates (D2, D3) are not required for v0.2.5. If the same session wants them, D1 is
  shared and the branch can carry both; the read-only gate is the part this extension is blocked on.

### R3 — a second batch on a streaming pinned connection fails cleanly

Defensive, independent of R2: with `threads > 1` the observed failure is a torn TDS stream, not the
`not in Idle state` error. `TdsConnection::ExecuteBatch`'s state check should hold under
concurrency (a mutex around state transition + send, or a per-connection lock held from batch start
to DONE so the second caller waits). R2 removes the trigger for catalog scans; R3 makes any future
one an error message instead of a corrupted connection.

### Release mechanics

`origin/duckdb-v1.5.5` is exactly the `v0.2.4` tag (same commit; duckdb submodule `d8cdaa33fd` =
v1.5.5, the pin this repository runs). `main` has moved to the duckdb 2.0 pre-release
(`chore: track duckdb's v2.0 pre-release branch`), so the work lands on `duckdb-v1.5.5` first:
branch from it, implement R1–R3 with tests, CHANGELOG `[0.2.5]`, tag `v0.2.5`, community
submission on the v1.5.5 line; then forward-port to `main` inside spec 066. **Done**: v0.2.5 is
tagged, community-extensions#2676 is open, and this repository's pin, tests and docs are on it —
`extension_config.cmake` at `GIT_TAG v0.2.5`, `METADATA_SCHEMA 'dbo'` gone from
`test/sql/integration/attach_mssql.test`, which now also pins the second-write failure.

### What v0.2.5 does not cover (from its CHANGELOG) — the constraint spec 004 inherits

The materialization gate counts *catalog* scans (three-part names, the joins and correlated
subqueries over them). `mssql_scan()` is a different table function on the same pinned connection
and is **not** materialized: inside a transaction, a plan that mixes `mssql_scan()` with a catalog
scan, or holds two `mssql_scan()`s, still fails the same way. For the manager this means an
`mssql_scan()` may only appear as the *sole* source of its query — fine for `GetLatestSnapshotQuery`
and for `Execute` (`mssql_exec` drains its batch before returning), not fine for
`GenerateFileColumnStatsCTEBody`, whose CTE lives inside a query that also joins the catalog's
tables. Either that read stays a catalog scan (materialized by v0.2.5), or the whole file-listing
query is pushed server-side as one `mssql_scan()`. Verified against the v0.2.5 build (2026-09-08):
`mssql_scan` + catalog scan in one plan and two `mssql_scan`s both fail inside a transaction, at
`threads` 1 and 4 and at any table size, while two *catalog* scans pass. Which shapes survive is
decided by the plan, not by drain order: a subquery that decorrelates into a plain build→probe
hash-join chain passes, and any shape that plans as `LEFT_DELIM_JOIN` fails, because the gate
counts `mssql_catalog_scan` only and materializes nothing. Do not read a passing example as a
rule. mssql-extension PR #314 closes the gap on
the duckdb 2.0 line, and that is where this repository picks it up — with the 2.0 bump, not a
backport. It does not block anything: on v1.5.5 the manager keeps `mssql_scan()` the sole source
of its query, which is how the hot reads are shaped anyway.

## Enforcement & security

No new trust surface. R2 only ever buffers data the same statement would have streamed; R1 changes
a default, not a permission. Across a version mismatch (this extension against an older mssql) the
attach itself fails, with `Schema 'main' not found in MSSQL database` — true but not actionable,
because the deps gate in `src/mssql_ducklake_extension.cpp` still only checks that *some* mssql is
loaded. Until it checks the version (Follow-ups), the README carries the requirement and the
`METADATA_SCHEMA 'dbo'` fallback, which matters while `INSTALL mssql FROM community` still serves
v0.2.4.

## Testing

In mssql-extension (`test/sql/transaction/`, beside `transaction_mssql_scan.test`; docker SQL Server
via `MSSQL_TEST_DSN`):

```sql
-- R2: the shape that fails on v0.2.4 (two tables, correlated subquery, explicit transaction)
statement ok
BEGIN;
query II
SELECT p.id, (SELECT list(c.v) FROM db.dbo.child c WHERE c.pid = p.id) FROM db.dbo.parent p ORDER BY 1;
statement ok
COMMIT;
-- the same with SET threads = 4 (the torn-stream variant), and a 3-scan plan
-- R1
statement ok
USE db;
query I
SELECT current_schema();
----
dbo
```

In this repository, after the pin bump — **landed**: `attach_mssql.test` attaches without
`METADATA_SCHEMA`, creates a table, inserts an inlined row pair, reads it back through the lake and
through `mssql_scan`, checks the inlined-table registration row, re-attaches, time-travels, and
pins the second-write failure with a `statement error`.

## Alternatives considered

- **Route every ducklake read through `mssql_scan` in the manager**: correct but it is the whole
  phase-3 transpiler, on the critical path, re-audited per ducklake bump. Rejected as the *only*
  fix; hot reads still go through `mssql_scan` for performance (spec 004).
- **Read on a second connection**: blocks on the transaction's own locks after the commit batch.
- **Lazy batch start (send on first `GetData`) instead of materialization**: keeps streaming but is
  only safe with a per-connection lock and single-threaded pipelines; more moving parts than the
  postgres precedent for no gain on small metadata tables. Fine as a later refinement of R2.
- **Wait for spec 066 on `main` and the 2.0 line**: this repository ships on released duckdb
  v1.5.5; a fix that exists only against a 2.0 pre-release does not reach it.

## Follow-ups

- Watch mssql-extension #268 (VARCHAR exposed as `MSSQL_VARCHAR` UDT): ducklake's inlined-data
  tables are created and read back through the catalog scan; if the UDT surfaces there,
  `TransformInlinedData` (spec 004) has to cast it, or v0.2.5 exposes plain `VARCHAR` under
  `mssql_catalog_native_types = false`.
- #140 (UPDATE without PK) is not needed: the manager's writes go through `mssql_exec`.
- **The version gate this spec assigned to the pin bump is not done.** The deps gate checks only
  that mssql is loaded, so a user on v0.2.4 (still what `INSTALL mssql FROM community` serves until
  community-extensions#2676 lands) gets `Schema 'main' not found in MSSQL database` with nothing
  naming the version. Deferred because the version is only legible at runtime: duckdb reports our
  build's git SHA in `duckdb_extensions().extension_version`, and the clean semver comes from
  `mssql_version()` — a query, which the load path cannot run safely. Two ways out, both spec 004
  work: run it from a fresh `Connection` at load, or check once in `MSSQLMetadataManager` at the
  first `ducklake:mssql:` attach, refusing only on a *known* older version. Until then README
  states the requirement and the fallback.
- **R3 shipped only inside R2** (see Status): a second batch on a streaming pinned connection still
  tears the stream instead of failing cleanly. Nothing in this repository depends on it — the
  manager keeps `mssql_scan()` the sole source of its query — but the R3 section above describes an
  intent, not the code.
- **`MSSQL_VARCHAR(MAX, 'collation')`** — filed as mssql-extension#321. Today a per-column MAX
  target with a stated collation does not exist (the binder caps `n` at 8000/4000), so the manager
  writes the inlined-table DDL itself and uses `COPY … (CREATE_TABLE false)`. When it lands on the
  2.0 line, those columns become an explicit cast and the hand-written DDL goes away.
