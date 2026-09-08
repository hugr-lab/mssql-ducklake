# Spec 002: the standalone extension — DuckLake on SQL Server with embedded ducklake

- **Status**: accepted (scaffold implemented; the metadata manager itself follows)
- **Date**: 2026-09-08
- **Author**: VGSML
- **Supersedes**: [001](../001-bridge-extension/spec.md)

## Summary

`mssql_ducklake` **compiles ducklake in** — the whole untouched pinned source — and adds a
`MSSQLMetadataManager` beside the built-in postgres/sqlite ones, so
`ATTACH 'ducklake:mssql://…'` keeps the DuckLake catalog in SQL Server through the `mssql`
extension. One image means the manager registry is ours by construction; the price is **mutual
exclusion with stock ducklake**. Shipped as an experimental extension on released DuckDB (v1.5.5)
through the community repository; an upstream PR of the manager into ducklake is a separate track,
taken up if the extension finds users.

## Problem

Spec 001's bridge assumed the loaded ducklake's manager registry is reachable from another
extension. Verified: it is not, by construction. `extension_build_tools.cmake:143` sets
`CXX_VISIBILITY_PRESET hidden` unconditionally for every loadable, so
`DuckLakeMetadataManager::Register` is hidden **at compile time** (it does not even appear in the
artifact's symbol table — inlined); a stock artifact exports exactly its entry point. Every channel
is closed: `dlsym` on the handle, `-undefined dynamic_lookup`, the global scope
(`RTLD_NOW|RTLD_LOCAL`), Windows `GetProcAddress`, and ducklake offers no SQL registration hook.
Checked on official artifacts v1.5.3–v1.5.5, on our own build, and on the `v2.0-cyanoptera` build
code — DuckDB 2.0 changes none of it.

A ducklake fork with the manager in-tree (plus upstream PR) works, but ties every manager
iteration to ducklake's release cycle. The remaining degrees of freedom: embed, or degrade to the
generic manager.

## Design

**Not coexistence — replacement.** The extension carries all of ducklake (submodule, build-only,
release-pinned, zero patches) plus the manager, in one image:

- **Build**: `add_subdirectory(ducklake/src)` yields ducklake's own `ALL_OBJECT_FILES`, compiled
  into our targets exactly as ducklake's build consumes them; `roaring` (its deletion-vector
  dependency) moves into our `vcpkg.json`. Nothing in the submodule is modified.
- **Load** (`src/mssql_ducklake_extension.cpp`), in order:
  1. **Exclusion gate**: stock `ducklake` loaded → refuse with a message naming the choice (the
     duplicated functions and `ducklake` ATTACH prefix cannot coexist, and each image reads its own
     registry).
  2. **Deps gate**: `mssql` loaded or autoloadable → else refuse naming the fix (the manager's SQL
     runs through `mssql_exec`/`mssql_scan`, resolved at runtime — nothing links mssql).
  3. `ducklake_duckdb_cpp_init(loader)` — ducklake's own `extern "C"` entry, same image: registers
     the full ducklake surface (ATTACH prefix, `ducklake_*` functions with their native names,
     secret type, settings).
  4. `DuckLakeMetadataManager::Register("mssql", MSSQLMetadataManager::Create)` under
     `std::call_once` — the registry is process-global while Load runs per `DatabaseInstance`, and
     a duplicate Register throws by design.
- **UX = stock**: nothing is renamed; `ATTACH 'ducklake:postgres:…'` works too (the postgres
  manager rides along), so one extension serves both lakes at once.
- **The autoload trap**: `ATTACH 'ducklake:…'` *before* our LOAD autoloads **stock** ducklake by
  prefix. Load this extension explicitly first — after it, the prefix is taken and no autoload
  fires. Documented in the README; the exclusion gate turns the collision into a clear error in
  the other order.

Bootstrap:

```sql
INSTALL mssql FROM community;  INSTALL mssql_ducklake FROM community;  -- once published
LOAD mssql_ducklake;
ATTACH 'ducklake:mssql:Server=…;Database=lake_meta;User Id=…;Password=…' AS lake
    (DATA_PATH 's3://…', METADATA_SCHEMA 'dbo');   -- `mssql://…` needs META_TYPE 'mssql' (README)
```

## Version pinning & vendoring

One release line, bumped together: duckdb `v1.5.5`, ducklake `v1.5-variegata` (embedded), mssql
`v0.2.4` (test loadable), extension-ci-tools `v1.5.5`. The ducklake submodule bumps on **our**
schedule — manager iterations never wait for a ducklake release — and the flip side is ours too:
ducklake fixes reach users with our bump, so bumps stay cheap and frequent (a bump = transpiler
closed-list re-audit + the smoke suite).

## Enforcement & security

Fail-closed both ways: no silent degradation to the generic manager (the mssql manager is always in
the image), no half-loaded state (either gate refuses before anything registers). The extension is
honest about what it is — the description and README say it embeds ducklake at a named pin and is
mutually exclusive with the stock extension.

## Testing

- `test/sql/mssql_ducklake.test`: production load order, the embedded ducklake surface is present,
  and a **full lake cycle with metadata in a local duckdb file** — ATTACH, table, insert, read,
  snapshots — no server needed, every step through the embedded copy.
- `test/sql/deps_gate.test`: the mssql refusal (autoload pinned off), then success beside mssql.
- `scripts/ci/smoke_load.sh`: out-of-tree CLI load — the local-file lake round trip, the mssql
  gate, and the **stock-ducklake exclusion gate** against a real `INSTALL ducklake` artifact
  (skipped with a note when offline).
- `test/sql/integration/attach_mssql.test`: a real SQL Server (docker/docker-compose.yml locally,
  a service container on CI's Linux job; gated on `MSSQL_DUCKLAKE_TEST_DSN`, which
  `make test-integration` exports and CI forbids skipping) — initialize a new catalog, read it
  back through `mssql_scan`, re-attach the existing one, the data-path pin.
- TODO (with the manager): DDL/DML/inlining against SQL Server; bench vs the postgres backend.

## Alternatives considered

- **Bridge extension registering into loaded ducklake** (spec 001): physically impossible —
  verified, see Problem.
- **Fork ducklake + upstream PR now**: clean for the community world but couples every manager
  iteration to ducklake's release cycle; kept as the *later* track — by traction — with the
  postgres manager as the in-tree precedent.
- **Generic manager + a smarter mssql catalog**: survives as a degradation story, but loses the
  Execute passthrough (performance) and the inlining type matrix.
- **Renaming the embedded surface to coexist with stock ducklake**: divergent behavior and a
  permanent rename layer; mutual exclusion is the honest version of the same idea.

## Follow-ups

- Spec 004+: the manager phases (research note §7) — T-SQL transpile of the closed statement set
  over `Execute`, own `InitializeDuckLake` (keys + filtered `WHERE end_snapshot IS NULL` indexes),
  then the server-side `ducklake_commit` procedure (single round-trip data-only commits).
- Two findings from the first live attach (2026-09-08), handed to mssql-extension as spec 003:
  1. `METADATA_SCHEMA 'dbo'` is required — asked for its default schema, the mssql catalog answers
     duckdb's `main` (an mssql-extension fix: override `GetDefaultSchema`).
  2. Through the generic manager, ATTACH initializes and re-opens a catalog in SQL Server, but the
     catalog load behind any DDL/DML fails: ducklake's reads with decorrelated subqueries
     (`LEFT_DELIM_JOIN` over `ducklake_view`/`ducklake_tag`) keep two mssql scans open on the one
     connection pinned to the transaction — the mssql scan runs its batch at source
     initialization, and the second source finds it streaming ("connection not in Idle state").
     Plain joins pass, the same query passes in autocommit. So the manager routes its reads
     through `mssql_scan` (one server-side statement, one result set each), not only the hot
     ones; an mssql-extension change (lazy or draining scan start on a pinned connection) is the
     complementary half — spec 003 makes it the primary one.
- Community submission (experimental), with the honest embeds-ducklake description.
- The upstream-PR track, opened when usage justifies it.
