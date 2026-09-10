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
  | mssql | `extension_config.cmake` (runtime pair, test loadable) | tag `v0.2.5` |
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
  mssql_metadata_manager.cpp     # the manager: type matrix, talking to the server, inlined table, attach probe
  mssql_catalog_shape.cpp        # what shapes a catalog: keys, indexes, collations, the database option (004, 006, 012)
  mssql_server_commit.cpp        # phase 2: the commit staged and applied on the server (005)
  mssql_metadata_queries.cpp     # the queries written in T-SQL: the conflict check (007), the read layer (008)
  include/                       # the class header, mssql_metadata_internal.hpp (switches + shared helpers)
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
website/                         # the docs site: Docusaurus, versioned per release, deployed to
                                 #   hugr-lab.github.io/mssql-ducklake by pages.yml (specs/013)
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
cp -n .env.example .env; make docker-up     # the SQL Server for the integration suite (creates the catalog db)
make test-integration                       # test/sql/integration/* with the DSN exported; `make test` skips them
make test-concurrent                        # concurrent writers (specs/007); the one regression sqllogictest cannot express
make metadata-log WORKLOAD=w.sql            # every metadata query DuckLake issued over w.sql, by shape, cost and path (specs/008)
find src \( -name '*.cpp' -o -name '*.hpp' \) | xargs clang-format -i   # pin: clang_format==11.0.1 (pip)
make format-check                           # duckdb's format.py over src + test (needs black, cmake-format, clang_format 11.0.1 in PATH)
make tidy-check                             # clang-tidy over src (TIDY_BINARY=... to pick one); what the distribution's code-quality job runs
cd website && npm ci && npx docusaurus build   # the docs site; broken links fail it (docs-build.yml is the PR gate)
```

Build outputs: CLI `build/release/duckdb`, loadables
`build/release/extension/{mssql_ducklake,mssql}/…​.duckdb_extension`, test binary
`build/release/test/unittest`.

**Test-runner loading rules** (they bit us once): `require <ext>` resolves only statically linked
extensions (parquet here) and duckdb's AUTOLOADABLE list — a `DONT_LINK` loadable is invisible to
it and the test silently SKIPS. Load those by build path:
`LOAD '__BUILD_DIRECTORY__/extension/mssql/mssql.duckdb_extension';`. In a gate test,
`SET autoload_known_extensions = false;` first. The static test shell loads static extensions
lazily — `require parquet` before ANY insert into a lake, inlined or not: both insert planners bind
parquet's copy function at plan time, before inlining is decided. `require-env
MSSQL_DUCKLAKE_TEST_DSN` gates the server-backed files: the Linux CI job provides it (it starts the
same `docker/docker-compose.yml` server and runs `make docker-up` + `make test-integration`) and
forbids the skip. CI's `scripts/ci/assert_ran.sh` floor keeps a silently-skipped suite from passing. A `.test`
file's `# group:` must equal its directory name (`[sql]`, `[integration]`) — duckdb's `format.py`
rewrites anything else and the distribution's format check fails on it.

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
- **The commit batch is our T-SQL, statement by known statement** (spec 014, `Execute` in
  `mssql_commit_batch.cpp`): DuckLake's batch is a closed list of shapes — captured off every write
  it can make, `design/003` — and each is recognised exactly and sent as the manager's own T-SQL,
  contiguous runs in one `mssql_exec` on the transaction's connection. No transpiler: a statement
  matches to the character or goes to the base — DuckDB's DML operators against the keyed catalog
  (spec 004 D3 is what makes that path work) — in order. The user's inlined rows always take the
  base path; `MSSQL_DUCKLAKE_STRICT_BATCH=1` (the suite) makes any other fallback an error naming
  the statement, which is how a ducklake bump that adds a shape is caught.
- **Inlining is in scope**: DuckLake inlines small inserts into catalog tables (default limit 10);
  the manager owns the inlined-table DDL/types via the type hooks. The matrix and edge cases
  (FLOAT NaN, TIMESTAMP_NS, HUGEINT, STRUCT) are in the research note §5.
- **Performance target: ≥ postgres backend.** Phase 1 parity (Execute passthrough,
  `GetLatestSnapshotQuery` via `mssql_scan`, own `InitializeDuckLake` with PKs + indexes,
  `MaxIdentifierLength=128`); phase 2 beats it with a server-side `ducklake_commit` T-SQL
  procedure — data-only commits in one round trip with server-side retry. Research note §7 is the
  plan; both phases are entirely ours (no upstream involved). Spec 006 then turned
  `SupportsAppender` on — measured, the appender does work against a remote catalog, contrary to
  what postgres and sqlite answering `false` suggested — and made the catalog's own storage
  `VARCHAR` under a UTF-8 BIN2 collation rather than `NVARCHAR`.
- **The catalog's database is shaped, once per shape version** (`EnsureCatalogShape`, stamped on
  `ducklake_metadata`): keys, the UTF-8 BIN2 `VARCHAR`s, indexes — and `PARAMETERIZATION FORCED`
  on the database itself, best-effort and with `mssql_ducklake_forced_parameterization = false` as
  the opt-out (specs/012). Without it every distinct literal in a query is a plan compile on the
  server: ~35 ms per table on its first touch (specs/009), a quarter of the 1000-table benchmark.
- **Both lakes at once**: `ducklake:postgres:` works through the embedded copy too — one extension
  serves postgres-cataloged and mssql-cataloged lakes in the same process.
- **Attach syntax**: `ducklake:mssql:<ADO connection string>` — duckdb strips `mssql:` as an
  extension prefix, but deliberately not `mssql://`, so the URI form needs `META_TYPE 'mssql'`.
  The catalog lands in `dbo` — the constant mssql ≥ v0.2.5 answers as the catalog's default schema
  (not the login's own; a login defaulting elsewhere still passes `METADATA_SCHEMA`). Older mssql
  answers duckdb's `main` and the attach fails; the pin is v0.2.5 for that reason (spec 003).
- **Where the generic manager stops, with mssql v0.2.5** (verified 2026-09-08): ATTACH
  initializes/re-opens the catalog, and a table's FIRST write commits — CREATE TABLE plus one
  insert, inlined (2 rows) or file-backed (100 rows → parquet); a DELETE against a file-backed
  table commits too (it writes a delete file, no stats row). Every LATER write to that table fails:
  once stats exist the commit batch UPDATEs `ducklake_table_stats`, that UPDATE takes duckdb's DML
  path, and mssql needs a PK for it. A DELETE of an inlined row fails the same way on
  `ducklake_inlined_data_<t>_<v>`. Exactly what the manager's primary keys remove (spec 004 D3).
  Pinned by `statement error` in the integration test.
- **`mssql_scan()` on the pinned connection**: v0.2.5 materializes only *catalog* scans; a plan
  mixing `mssql_scan()` with a catalog scan, or two `mssql_scan()`s, inside a transaction fails
  **depending on execution order** — measured on 20,000-row results (design 002 §5), a
  `catalog JOIN mssql_scan` passes when the plan happens to drain the stream first and the same
  pair fails as a CTE, so one lucky plan proves nothing. The fix (mssql-extension #314) arrives
  with the duckdb 2.0 line; not a blocker — on v1.5.5 the manager uses `mssql_scan()` only as the
  sole source of a query.

## Documentation

`website/` is the user documentation — a Docusaurus site mirroring hugr-lab/mssql-extension's
(same look, same versioning contract), published at https://hugr-lab.github.io/mssql-ducklake/ by
`.github/workflows/pages.yml` on every push to `main` that touches it; `docs-build.yml` builds it
on pull requests. The org site (hugr-lab.github.io) links here. Rules that came from the mssql
extension's site: links between pages are relative markdown links, never absolute site paths
(`scripts/ci/check_docs_links.py` enforces it — an absolute link leaves the version the reader is
in); **at each release run `npm run docusaurus docs:version <X.Y.Z>` in `website/` and commit the
snapshot**, so the released docs serve at the root and the live tree as *Next*. A feature that
changes what a user sees lands with its page in `website/docs/`, the way it lands with its spec.

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
