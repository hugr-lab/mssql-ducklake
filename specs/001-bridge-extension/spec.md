# Spec 001: the bridge extension — DuckLake metadata catalog on SQL Server

- **Status**: accepted (scaffold implemented; manager registration pending)
- **Date**: 2026-09-08
- **Author**: VGSML

## Summary

`mssql_ducklake` is a bridge extension: it contributes a `MSSQLMetadataManager` to DuckLake's
metadata-manager registry, so `ATTACH 'ducklake:mssql://…'` keeps its catalog in SQL Server through
the `mssql` extension. Neither side changes: `mssql` ships as today with no ducklake code or build
coupling, `ducklake` stays untouched upstream source. The bridge is the only coupled piece, and it
is small — a metadata manager, like the postgres one, only *generates SQL* (`mssql_exec('…', sql)`
resolves at runtime), so the bridge needs no mssql code either.

## Problem

DuckLake resolves its metadata manager by catalog-path prefix (`ducklake:<type>:…`) through a
process-global registry pre-seeded with postgres/sqlite/quack. An unknown type silently gets the
generic manager: no SQL Server dialect, no PK-friendly schema, no server-side commit. Registering a
manager requires calling `DuckLakeMetadataManager::Register` — which lives *inside* the ducklake
image.

The linkage model makes this the whole problem (verified empirically on built artifacts): a loadable
duckdb extension statically embeds duckdb — and would embed ducklake — into itself, with everything
but its entry point hidden. A manager compiled into some other extension registers into a *dead
copy* of the registry; the real `Create()` runs in ducklake's image and reads ducklake's map. The
official ducklake artifact exports exactly one symbol (its entry point), so the real registry is
unreachable by linking and by `dlsym` alike.

## Design

**Load of the bridge IS the registration** — loading it is the explicit opt-in, no separate call:

1. **Deps gate**: `ExtensionIsLoaded("ducklake")` / `ExtensionHelper::TryAutoLoadExtension`, same
   for `mssql`; either absent → `MissingExtensionException` naming the fix
   (`INSTALL x; LOAD x; LOAD mssql_ducklake`). *(Implemented in the scaffold.)*
2. **Version gate**: the loaded ducklake's `extension_version` must equal the version the bridge was
   built against — the cross-image surface is a C++ vtable, so this is ABI hygiene, not pedantry.
3. **Registration**: in a loadable build (`#ifdef DUCKDB_BUILD_LOADABLE_EXTENSION`),
   `dlopen(ducklake path, RTLD_NOLOAD)` → `dlsym` the exported `Register` → call into the REAL
   registry; in a static build, a direct `DuckLakeMetadataManager::Register` call. Guarded by
   `call_once` (the registry is process-global, Load runs per `DatabaseInstance`); never from a
   static initializer (SIOF).

**Requirement on the ducklake artifact**: `Register` must be exported. Stock builds hide it, so the
paired ducklake is distributed from the hugr extension repository — untouched upstream source plus
one linker line (`exported_symbols_list` with a single mangled name; `/EXPORT` on Windows).

**Composition of the bridge**: `MSSQLMetadataManager` + the ducklake base implementations it
overrides, compiled from the `ducklake/` submodule (build-only dependency, pinned to the same
release the co-loaded artifact is built from). The cross-image surface is exactly one `Register`
call; after that ducklake drives our factory and virtuals through the object's vtable.

**Load the bridge before the first `ducklake:mssql:` ATTACH**: the first attach runs
`InitializeDuckLake`/`ProbeServerCapabilities` through whichever manager resolves — if the bridge
came late, the generic manager has silently created a schema without our indexes/procedure. Load
detects an existing `ducklake:mssql` attach and warns.

Bootstrap a user runs:

```sql
INSTALL ducklake FROM hugr;  INSTALL mssql FROM hugr;  INSTALL mssql_ducklake FROM hugr;
LOAD mssql_ducklake;          -- autoloads deps, gates the version, registers the manager
ATTACH 'ducklake:mssql://…?database=lake_meta' AS lake (DATA_PATH 's3://…');
```

Support matrix: hugr repo (all three) — full experience; a static worker build — full (bridge linked
in, direct `Register`); stock ducklake from the core repo — registry unreachable, the bridge refuses
at its gate and the generic manager remains the fallback. The eventual community fix is upstreaming
a C-ABI registration hook to ducklake — an option, not a dependency.

## Version pinning

The whole stack rides one release line, bumped together:

| Piece | Pin |
| --- | --- |
| duckdb (submodule) | `v1.5.5` |
| ducklake (submodule, build-only) | branch `v1.5-variegata` |
| mssql (test loadable) | tag `v0.2.4` (built against duckdb v1.5.5) |
| extension-ci-tools (submodule) | branch `v1.5.5` |

The research behind this spec (`design/001-ducklake-mssql-metadata/RESEARCH.md`, local) was verified
against ducklake/duckdb `main` of 2026-08-31; its file:line references predate this pin. Re-verify
the touched seams against `v1.5-variegata` as part of implementing the manager.

## Enforcement & security

Fail-closed: the bridge refuses to load without both sides, and refuses to register across a
ducklake version it was not built against. No fallback that silently degrades: when the gate
refuses, DuckLake behavior is exactly what it was without the bridge.

## Testing

- `test/sql/mssql_ducklake.test`: production load order — `require ducklake` (static), then mssql
  and the bridge by build path (`LOAD '__BUILD_DIRECTORY__/…'`, the runner's pattern for DONT_LINK
  loadables) — and the version function answers.
- `test/sql/deps_gate.test`: the refusal itself, with autoload pinned off — no sides → the error
  names ducklake; ducklake only → it names mssql; both → the same LOAD succeeds.
- `scripts/ci/smoke_load.sh`: the built loadable, copied out of tree, (a) loads beside mssql and
  answers, (b) *refuses with the gate's own message* when mssql is absent — a missing-symbol failure
  or crash would surface here instead.
- TODO (with the manager): a C++ test for the version gate; integration tests against a real SQL
  Server (docker) covering ATTACH/commit/inlining.

## Alternatives considered

- **Manager inside the mssql extension** (register on load or via a function): couples mssql to
  ducklake's C++ ABI for every user, including the majority who never use DuckLake; and a loadable
  mssql cannot reach a stock ducklake's registry anyway. Rejected — the bridge keeps both sides
  independent.
- **PR to ducklake**: fastest technically, rejected as a hard constraint (no upstream changes);
  revisit only as the optional C-ABI registration upstream.
- **Generic manager + identity-column workarounds**: survives as the no-bridge fallback path, but
  cannot fix type mapping for inlined tables nor reach postgres-level performance.

## Follow-ups

- Spec 002+: the manager itself — Phase 1 parity with the postgres manager (Execute passthrough via
  `mssql_exec`, T-SQL transpile of the closed statement set, own `InitializeDuckLake` with PKs +
  filtered indexes, `MaxIdentifierLength=128`, `SupportsAppender=false`), Phase 2 server-side
  commit procedure (quack-style single round-trip), Phase 3 hot reads. See the research note §7/§8.
- The hugr ducklake build with the exported `Register` (repo-side, one linker line).
