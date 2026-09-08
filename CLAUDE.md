# mssql-ducklake — Development Guidelines

A **bridge extension** for DuckDB: it registers a `MSSQLMetadataManager` into DuckLake's
metadata-manager registry, so `ATTACH 'ducklake:mssql://…'` keeps the DuckLake catalog in Microsoft
SQL Server through the `mssql` extension. Neither side changes — `mssql` ships as today with no
ducklake code or build coupling, `ducklake` stays untouched upstream source. Loading the bridge IS
the registration; by contract it **fails fast** when `ducklake` or `mssql` is not loaded.

Read **[specs/001-bridge-extension/spec.md](specs/001-bridge-extension/spec.md)** for the core
model. Deeper research/thinking lives in a local `design/` folder (gitignored) — start with
`design/001-ducklake-mssql-metadata/RESEARCH.md` (the founding research: registry, linkage model,
the full manager plan).

## Technology

- **Language**: C++17 (DuckDB extension standard).
- **DuckDB**: pinned to the **latest release line**, never `main` — the bridge pairs at runtime with
  *distributed* artifacts of ducklake and mssql, and a loadable extension statically embeds duckdb
  (exact-version match required). All pins ride one line and are bumped **together, in one commit**:

  | Piece | Where | Pin |
  | --- | --- | --- |
  | duckdb | submodule `duckdb/` | tag `v1.5.5` |
  | ducklake | submodule `ducklake/` (build-only dep) | branch `v1.5-variegata` (SHA in the submodule) |
  | mssql | `extension_config.cmake` (test loadable) | tag `v0.2.4` (built against duckdb v1.5.5) |
  | extension-ci-tools | submodule `extension-ci-tools/` | branch `v1.5.5` |
  | CI reusable workflows | `.github/workflows/distribution.yml` | `@v1.5.5`, `duckdb_version: v1.5.5` |

  On a bump: re-verify the ducklake seams the manager overrides (the vtable is the ABI), and
  re-audit the T-SQL transpiler's closed statement list against the new ducklake (research note §8).
- **Dependencies**: none of our own; the build's vcpkg deps arrive through the **merged manifests**
  of the loaded extensions (roaring from ducklake, openssl/simdutf from mssql).
- **Platforms**: Linux, macOS, Windows; **no wasm** (the loadable path is dlopen/dlsym, mssql is raw
  TDS sockets).

## Project structure

```text
src/
  mssql_ducklake_extension.cpp   # entry: the deps gate; (spec 002+) version gate + registration
  include/                       # mssql_ducklake_extension.hpp
ducklake/                        # submodule, BUILD-ONLY dep: headers + base impls the manager overrides
extension_config.cmake           # what the duckdb build loads: bridge (DONT_LINK), ducklake (static),
                                 #   mssql (DONT_LINK loadable @ release tag)
test/sql/
  mssql_ducklake.test            # the happy path in production load order
  deps_gate.test                 # the refusal itself: no sides / one side / both
scripts/ci/                      # smoke_load.sh, assert_ran.sh, prune_vcpkg_cache.sh
specs/                           # one lightweight spec per feature, NNN-slug/spec.md (see specs/README.md)
design/                          # LOCAL, gitignored: numbered research topics NNN-topic/ (our scratch)
```

## Commands

```sh
git submodule update --init --recursive
make vcpkg-setup                    # once (or point VCPKG_TOOLCHAIN_PATH at a sibling repo's vcpkg)
GEN=ninja make                      # release build: duckdb CLI + unittest + ducklake (static)
                                    #   + mssql & the bridge (loadables)
GEN=ninja make debug                # debug build

build/release/test/unittest 'test/sql/*'    # the sqllogictest suite (what CI runs)
scripts/ci/smoke_load.sh                    # the loadable OUT of tree: loads beside mssql,
                                            #   refuses without it with the gate's own message
find src \( -name '*.cpp' -o -name '*.hpp' \) | xargs clang-format -i   # pin: clang_format==11.0.1 (pip)
```

Build outputs: CLI `build/release/duckdb`, loadables
`build/release/extension/{mssql_ducklake,mssql}/…​.duckdb_extension`, test binary
`build/release/test/unittest`, and a local extension repository under `build/release/repository/`.

**Test-runner loading rules** (they bit us once): sqllogictest's `require <ext>` resolves only
statically linked extensions and duckdb's AUTOLOADABLE list — a `DONT_LINK` loadable is invisible to
it and the test silently SKIPS. Load those by build path instead:
`LOAD '__BUILD_DIRECTORY__/extension/mssql/mssql.duckdb_extension';` (the runner substitutes the
token and allows unsigned). In a negative/gate test, `SET autoload_known_extensions = false;` first,
so a configured local repo cannot quietly satisfy the dependency under test. CI's
`scripts/ci/assert_ran.sh` floor exists precisely so a silently-skipped suite can never pass.

## Code style

- Follow DuckDB's conventions: tabs for indentation, ≤120 columns, `[u]int(8..64)_t` and `idx_t`,
  `unique_ptr`/`optional_ptr`/`reference`, never raw pointers or `const_cast`, braces always, short
  comments. Run `clang-format` (the repo `.clang-format`, formatter pin 11.0.1) before committing.
- Names: files `snake_case`, types `PascalCase`, functions `PascalCase`, variables `snake_case`.
- Prefer sqllogictest (`test/sql/*.test`) over C++ tests. Every feature lands with tests.

## Key concepts

- **The linkage model is the whole problem** (spec 001, verified empirically): a loadable extension
  statically embeds duckdb — and would embed ducklake — into itself, with everything but its entry
  point hidden. A manager compiled into another extension registers into a **dead copy** of the
  registry; the real `Create()` runs in ducklake's image and reads ducklake's map. Stock ducklake
  exports exactly one symbol, so its registry is unreachable by linking and `dlsym` alike.
- **Load of the bridge = registration**, three steps: (1) deps gate — `ExtensionIsLoaded` /
  `TryAutoLoadExtension` for ducklake and mssql, `MissingExtensionException` naming the fix;
  (2) version gate — the loaded ducklake's `extension_version` must equal the build-against version
  (the cross-image surface is a C++ vtable); (3) registration — direct
  `DuckLakeMetadataManager::Register` in a static build, `dlopen(RTLD_NOLOAD)`+`dlsym` of the
  *exported* `Register` in a loadable build. `call_once`; never from a static initializer.
- **The manager only generates SQL** — like the postgres manager it never links its scanner; the
  generated `mssql_exec('…', sql)` / `mssql_scan` calls resolve at runtime. That is why the bridge
  needs no mssql code and mssql needs no bridge code.
- **Execute-passthrough kills the PK problem**: every ducklake write (the commit batch, inlined-data
  flush, expire/cleanup) flows through one `Execute` seam; passed through as raw T-SQL server-side,
  duckdb's DML path (rowid/PK) is never involved — mssql's PK-required UPDATE/DELETE limitation
  vanishes for ducklake, without touching either repo.
- **Inlining is in scope, not disabled**: DuckLake inlines small inserts into catalog tables by
  default (limit 10); the manager owns the inlined-table DDL/types via the type hooks
  (`TypeIsNativelySupported`/`GetColumnTypeInternal`/`CastColumnToTarget`/…). The type matrix and
  edge cases (FLOAT NaN, TIMESTAMP_NS, HUGEINT, STRUCT) are in the research note §5.
- **Performance target: ≥ postgres backend.** Phase 1 = parity (Execute passthrough of the batched
  commit, `GetLatestSnapshotQuery` via `mssql_scan`, own `InitializeDuckLake` T-SQL with PKs +
  filtered indexes, `MaxIdentifierLength=128`, `SupportsAppender=false`). Phase 2 = beat it with a
  quack-style server-side `ducklake_commit` procedure — data-only commits in one round trip with
  server-side retry, which postgres does not have. Research note §7 is the plan.
- **Load the bridge before the first `ducklake:mssql:` ATTACH** — otherwise the generic manager
  silently creates a schema without our keys/indexes/procedure; Load should detect and warn.
- **Fail closed, degrade honestly**: when the gate refuses (missing side, version mismatch, symbol
  unreachable — e.g. stock ducklake), DuckLake behaves exactly as it would without the bridge.

## Distribution

- **Now (duckdb v1.5.5)**: through the **community extensions repository** — v1.5.5 has no external
  extension repositories, so `INSTALL mssql_ducklake FROM community` (and `INSTALL mssql FROM
  community`) is the channel; ducklake comes from the official repo. The loadable bridge pairs with
  a ducklake build that *exports* its registration symbol — untouched upstream plus one linker line
  — shipped as a release artifact of this repo and loaded explicitly; with stock ducklake the bridge
  refuses at its gate. A static (bundled) build needs no export.
- **With duckdb 2.0**: external extension repositories with per-repo keys land
  (`CREATE EXTENSION REPOSITORY hugr …` / `INSTALL … FROM hugr`); the hugr repository then serves
  all three (paired ducklake included) and becomes the primary channel. The README's bootstrap
  reflects whichever is current.

## Working process — per-feature specs

We do **not** run full spec-kit. Instead, each feature gets one lightweight spec under `specs/` (see
**[specs/README.md](specs/README.md)**):

1. Before (or alongside) implementing a feature, create `specs/NNN-slug/spec.md` from
   `specs/TEMPLATE.md` — problem, design, enforcement/security, tests, alternatives.
2. Implement with tests; keep the spec updated; set its status to `implemented` when done.
3. Reference the spec in the commit/PR.

Keep specs short and honest. When a decision changes, update the spec or supersede it with a new
one. `design/` (gitignored) is our scratch space for the research behind a spec. Do not commit
`design/`; do not push without being asked.

## Reference repos (local)

- `~/projects/hugr-lab/mssql-extension` — the mssql side: `mssql_exec`/`mssql_scan` (transaction
  pinning), type codecs (`src/codec/`), DML/PK constraints.
- `~/projects/duckdb/ducklake` — upstream ducklake clone; the manager surface is
  `src/include/storage/ducklake_metadata_manager.hpp`, the perf references are
  `src/metadata_manager/{postgres,quack}_metadata_manager.cpp` (parity and ceiling, respectively).
- `~/projects/hugr-lab/duckdb-acl` — sibling extension repo whose build/CI/process conventions this
  repo follows.
