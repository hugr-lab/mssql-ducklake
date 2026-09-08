# mssql-ducklake — Development Guidelines

**DuckLake on SQL Server, batteries included.** This extension **compiles ducklake in** — the whole
untouched pinned source — and adds a `MSSQLMetadataManager` beside the built-in postgres/sqlite
ones, so `ATTACH 'ducklake:mssql:…'` keeps the DuckLake catalog in SQL Server through the `mssql`
extension. One image means the manager registry is ours by construction; the price is **mutual
exclusion with stock ducklake** (same functions, same ATTACH prefix — one or the other, never
both). `mssql` itself ships as today, untouched; nothing here links it (the manager only generates
SQL, resolved at runtime through `mssql_exec`/`mssql_scan`).

Read **[specs/002-embedded-ducklake/spec.md](specs/002-embedded-ducklake/spec.md)** for the core
model — including why the bridge design of spec 001 was physically impossible (compile-time hidden
symbols; the verified findings are in there). Deeper research lives in the local, gitignored
`design/` folder — start with `design/001-ducklake-mssql-metadata/RESEARCH.md` (registry, linkage
model, the full manager plan §7: transpiler, keys + filtered indexes, server-side commit).

## Technology

- **Language**: C++17 (DuckDB extension standard).
- **Pins** — one release line, bumped together in one commit:

  | Piece | Where | Pin |
  | --- | --- | --- |
  | duckdb | submodule `duckdb/` | tag `v1.5.5` |
  | ducklake | submodule `ducklake/` (EMBEDDED — compiled into the extension) | branch `v1.5-variegata` (SHA in the submodule) |
  | mssql | `extension_config.cmake` (runtime pair, test loadable) | tag `v0.2.4` |
  | extension-ci-tools | submodule `extension-ci-tools/` | branch `v1.5.5` |
  | CI reusable workflows | `.github/workflows/distribution.yml` | `@v1.5.5`, `duckdb_version: v1.5.5` |

  **Vendoring rule**: the ducklake submodule bumps on OUR schedule (manager work never waits for a
  ducklake release), and ducklake fixes reach users only with our bump — so bumps stay cheap and
  frequent. A bump = re-audit of the T-SQL transpiler's closed statement list (research note §8) +
  the full test/smoke run.
- **Dependencies**: `roaring` (ducklake's deletion vectors) in our `vcpkg.json`; mssql's
  openssl/simdutf arrive through the merged vcpkg manifests.
- **Platforms**: Linux, macOS, Windows; **no wasm** (mssql is raw TDS sockets).

## Project structure

```text
src/
  mssql_ducklake_extension.cpp   # entry: exclusion gate (stock ducklake), mssql deps gate,
                                 #   ducklake_duckdb_cpp_init chain, Register("mssql") via call_once
  mssql_metadata_manager.cpp     # the SQL Server metadata manager (specs/004+ fill the phases)
  include/                       # mssql_ducklake_extension.hpp, mssql_metadata_manager.hpp
ducklake/                        # submodule, EMBEDDED: add_subdirectory(ducklake/src) supplies
                                 #   ALL_OBJECT_FILES; never modified, never loaded separately
extension_config.cmake           # loads: this extension (DONT_LINK) + mssql (DONT_LINK @ release tag)
test/sql/
  mssql_ducklake.test            # embedded surface + a full local-file lake cycle (no server)
  deps_gate.test                 # the mssql refusal, then success beside mssql
  integration/                   # server-backed suite, gated on MSSQL_DUCKLAKE_TEST_DSN (make test-integration)
docker/                          # the integration SQL Server: compose (everything named mssql-ducklake-*, port
                                 #   7433, pinned image) + init/sqlserver.sql (catalog db + test marker); .env.example
scripts/ci/                      # smoke_load.sh (incl. the stock-ducklake exclusion), assert_ran.sh, ...
specs/                           # one lightweight spec per feature, NNN-slug/spec.md (see specs/README.md)
design/                          # LOCAL, gitignored: numbered research topics NNN-topic/
```

## Commands

```sh
git submodule update --init --recursive
make vcpkg-setup                    # once (or point VCPKG_TOOLCHAIN_PATH at a sibling repo's vcpkg)
GEN=ninja make                      # release: duckdb CLI + unittest + mssql & this extension (loadables)
GEN=ninja make debug

build/release/test/unittest 'test/sql/*'    # the sqllogictest suite (what CI runs)
scripts/ci/smoke_load.sh                    # out-of-tree CLI: local-file lake round trip, both gates
cp .env.example .env && make docker-up      # the SQL Server for the integration suite (creates lake_meta)
make test-integration                       # test/sql/integration/* with the DSN exported; `make test` skips them
find src \( -name '*.cpp' -o -name '*.hpp' \) | xargs clang-format -i   # pin: clang_format==11.0.1 (pip)
```

Build outputs: CLI `build/release/duckdb`, loadables
`build/release/extension/{mssql_ducklake,mssql}/…​.duckdb_extension`, test binary
`build/release/test/unittest`.

**Test-runner loading rules** (they bit us once): `require <ext>` resolves only statically linked
extensions (parquet here) and duckdb's AUTOLOADABLE list — a `DONT_LINK` loadable is invisible to
it and the test silently SKIPS. Load those by build path:
`LOAD '__BUILD_DIRECTORY__/extension/mssql/mssql.duckdb_extension';`. In a gate test,
`SET autoload_known_extensions = false;` first. The static test shell loads static extensions
lazily — `require parquet` before a lake writes data files. `require-env MSSQL_DUCKLAKE_TEST_DSN`
gates the server-backed files: the Linux CI job provides it (service container) and forbids the
skip. CI's `scripts/ci/assert_ran.sh` floor keeps a silently-skipped suite from passing.

## Code style

- DuckDB's conventions: tabs for indentation, ≤120 columns, `[u]int(8..64)_t` and `idx_t`,
  `unique_ptr`/`optional_ptr`/`reference`, never raw pointers or `const_cast`, braces always, short
  comments. `clang-format` (formatter pin 11.0.1) before committing.
- Names: files `snake_case`, types `PascalCase`, functions `PascalCase`, variables `snake_case`.
- Prefer sqllogictest; every feature lands with tests. The embedded ducklake sources are NEVER
  edited — anything ducklake-shaped we need goes through manager virtuals or waits for a bump.

## Key concepts

- **Why embedded** (spec 002): a loaded stock ducklake's manager registry is unreachable from any
  other image — `CXX_VISIBILITY_PRESET hidden` is applied at compile time to every loadable, so
  `Register` isn't even in the symbol table, and every channel (dlsym, dynamic_lookup, global
  scope, GetProcAddress) is closed. The "dead copy" problem inverts when the copy is the ONLY one:
  `Create()` runs in our image and reads our registry.
- **Load order of the gates**: stock-ducklake exclusion → mssql deps check →
  `ducklake_duckdb_cpp_init(loader)` (full ducklake surface, native names) →
  `DuckLakeMetadataManager::Register("mssql", …)` under `std::call_once` (process-global map,
  per-instance Load, duplicate Register throws).
- **The autoload trap**: `ATTACH 'ducklake:…'` before our LOAD autoloads STOCK ducklake by prefix.
  This extension is loaded explicitly first; after that the prefix is taken and no autoload fires.
- **The manager only generates SQL** — like the postgres manager (the in-tree precedent) it never
  links its scanner; `mssql_exec('…', sql)` resolves at runtime.
- **Execute-passthrough kills the PK problem**: every ducklake write (commit batch, inlined flush,
  expire/cleanup) flows through one `Execute` seam; passed through as raw T-SQL server-side,
  duckdb's DML path (rowid/PK) is never involved — mssql's PK-required UPDATE/DELETE limitation
  never applies to the lake catalog.
- **Inlining is in scope**: DuckLake inlines small inserts into catalog tables (default limit 10);
  the manager owns the inlined-table DDL/types via the type hooks. The matrix and edge cases
  (FLOAT NaN, TIMESTAMP_NS, HUGEINT, STRUCT) are in the research note §5.
- **Performance target: ≥ postgres backend.** Phase 1 parity (Execute passthrough,
  `GetLatestSnapshotQuery` via `mssql_scan`, own `InitializeDuckLake` with PKs + filtered
  `WHERE end_snapshot IS NULL` indexes, `MaxIdentifierLength=128`, `SupportsAppender=false`);
  phase 2 beats it with a server-side `ducklake_commit` T-SQL procedure — data-only commits in one
  round trip with server-side retry. Research note §7 is the plan; both phases are entirely ours
  (no upstream involved).
- **Both lakes at once**: `ducklake:postgres:` works through the embedded copy too — one extension
  serves postgres-cataloged and mssql-cataloged lakes in the same process.
- **Attach syntax**: `ducklake:mssql:<ADO connection string>` — duckdb strips `mssql:` as an
  extension prefix, but deliberately not `mssql://`, so the URI form needs `META_TYPE 'mssql'`.
  `METADATA_SCHEMA 'dbo'` is required until the mssql catalog answers its real default schema
  (it returns duckdb's `main`; an mssql-extension fix).
- **What the generic manager gets on a live server** (2026-09-08): ATTACH initializes and re-opens
  a catalog in SQL Server, but DDL/DML fail — ducklake's catalog-load reads with decorrelated
  subqueries (LEFT_DELIM_JOIN over `ducklake_view`/`ducklake_tag`) keep two mssql scans open on
  the one connection pinned to the transaction (the scan runs its batch at source init) →
  "connection not in Idle state". Plain joins pass; the same query passes in autocommit. Phase 1
  therefore needs mssql v0.2.5 (spec 003: materialize multi-scan plans on a pinned connection,
  default schema `dbo`); the manager's own reads via `mssql_scan` stay the perf story (research note §9).

## Distribution

Experimental, through the **community extensions repository** on released DuckDB (v1.5.5). The
description is honest: embeds ducklake at a named pin, mutually exclusive with the stock ducklake
extension. The upstream track — a PR contributing the manager in-tree to ducklake (postgres-manager
precedent) — is taken up if the extension finds users; after such a merge this extension becomes a
deprecation shim.

## Working process — per-feature specs

We do **not** run full spec-kit. Each feature gets one lightweight spec under `specs/` (see
**[specs/README.md](specs/README.md)**): write it before or alongside the work from
`specs/TEMPLATE.md`, implement with tests, keep it current, supersede rather than rewrite history.
`design/` (gitignored) is the research scratch behind the specs. Do not commit `design/`; do not
push without being asked.

## Reference repos (local)

- `~/projects/hugr-lab/mssql-extension` — the runtime pair: `mssql_exec`/`mssql_scan` (transaction
  pinning), type codecs (`src/codec/`), DML/PK constraints.
- `~/projects/duckdb/ducklake` — upstream ducklake clone (main); the embedded pin lives in the
  `ducklake/` submodule here. Manager surface: `src/include/storage/ducklake_metadata_manager.hpp`;
  perf references: `src/metadata_manager/{postgres,quack}_metadata_manager.cpp`.
- `~/projects/hugr-lab/duckdb-acl` — sibling extension repo whose build/CI/process conventions this
  repo follows.
