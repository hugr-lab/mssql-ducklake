# Spec 003: mssql-extension v0.2.5 — what the DuckLake manager needs from the runtime pair

- **Status**: accepted — handed to `hugr-lab/mssql-extension` (branch `duckdb-v1.5.5`, release
  `v0.2.5`); this repository bumps its pin when it ships
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
`Schema 'main' not found in MSSQL database`. Today's workaround is `METADATA_SCHEMA 'dbo'` on every
ATTACH; mssql-extension issue #129 shows a user working around the same thing by creating a schema
called `main` in SQL Server.

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
submission on the v1.5.5 line; then forward-port to `main` inside spec 066. This repository bumps
`extension_config.cmake` (`GIT_TAG v0.2.5`), drops `METADATA_SCHEMA 'dbo'` from
`test/sql/integration/attach_mssql.test` and extends it with the lake DDL/DML that fails today.

## Enforcement & security

No new trust surface. R2 only ever buffers data the same statement would have streamed; R1 changes
a default, not a permission. Across a version mismatch (this extension against mssql v0.2.4) the
behavior is the documented one: the attach works, the first lake DDL fails with the message above,
and README says `METADATA_SCHEMA 'dbo'` is required. Once the pin is v0.2.5, the deps gate in
`src/mssql_ducklake_extension.cpp` can require that version.

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

In this repository, after the pin bump: `attach_mssql.test` without `METADATA_SCHEMA`, plus
`CREATE TABLE lake.t`, `INSERT`, `SELECT`, `ducklake_snapshots` — the smoke that motivated the spec.

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
