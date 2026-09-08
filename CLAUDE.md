# CLAUDE.md

Guidance for coding agents working in this repository.

## What this is

`mssql_ducklake` is a **bridge extension** for DuckDB: it registers a `MSSQLMetadataManager` into
DuckLake's metadata-manager registry so `ATTACH 'ducklake:mssql://…'` keeps the DuckLake catalog in
SQL Server (through the `mssql` extension). Neither side changes: `mssql` ships as today, `ducklake`
stays untouched upstream. Loading the bridge IS the registration; it fails fast (by contract) when
`ducklake` or `mssql` is not loaded. Start with `specs/001-bridge-extension/spec.md`.

Key consequence of the linkage model (loadable extensions statically embed duckdb, everything but
the entry point hidden): the bridge reaches the REAL registry via a direct call in static builds and
via `dlopen(RTLD_NOLOAD)`+`dlsym` of an *exported* `Register` in loadable builds — which is why the
paired ducklake artifact is distributed from the hugr repo with that one symbol exported.

## Version policy — release-pinned, bumped together

The whole stack rides the latest DuckDB release line. Never mix lines; bump all pins in one commit:

| Piece | Where | Pin |
| --- | --- | --- |
| duckdb | submodule `duckdb/` | tag `v1.5.5` |
| ducklake | submodule `ducklake/` (build-only dep) | branch `v1.5-variegata` |
| mssql | `extension_config.cmake` (test loadable) | tag `v0.2.4` |
| extension-ci-tools | submodule `extension-ci-tools/` | branch `v1.5.5` |
| CI reusable workflows | `.github/workflows/distribution.yml` | `@v1.5.5`, `duckdb_version: v1.5.5` |

## Build

```bash
make vcpkg-setup            # once: local vcpkg checkout (deps: roaring via ducklake, openssl/simdutf via mssql)
GEN=ninja make              # release build: duckdb CLI + unittest + ducklake (static) + mssql & bridge (loadables)
GEN=ninja make debug        # debug build
```

Artifacts land in `build/release/` (`duckdb`, `test/unittest`,
`extension/mssql_ducklake/mssql_ducklake.duckdb_extension`,
`extension/mssql/mssql.duckdb_extension`). ducklake is statically linked into the CLI/unittest,
mssql and the bridge are loadables — that mirrors production, where the bridge is always loaded
(`DONT_LINK` in `extension_config.cmake`; a statically linked bridge would run its deps gate at
every database startup).

## Test

```bash
build/release/test/unittest 'test/sql/*'     # sqllogictest suite
scripts/ci/smoke_load.sh                     # the loadable, out of tree: loads beside mssql, refuses without it
```

Tests follow duckdb's sqllogictest format. Load order in tests is the production order:
`require ducklake`, `require mssql`, then `require mssql_ducklake`.

## Format

```bash
find src \( -name '*.cpp' -o -name '*.hpp' \) | xargs clang-format -i   # pin: clang_format==11.0.1 (pip)
```

Tabs for indentation, 120 columns, duckdb naming (PascalCase functions, snake_case variables,
`idx_t` for counts, no raw pointers). Comments short, no change-history comments.

## Layout

```
src/                       # the bridge: deps gate + (spec 002+) the metadata manager
ducklake/                  # submodule, BUILD-ONLY dependency: headers + base impls the manager overrides
extension_config.cmake     # what the duckdb build loads: bridge (DONT_LINK), ducklake (static), mssql (loadable)
test/sql/                  # sqllogictests
scripts/ci/                # smoke_load.sh, assert_ran.sh, prune_vcpkg_cache.sh
specs/                     # one lightweight spec per feature (see specs/README.md) — write/update alongside work
design/                    # local research scratch, gitignored (see design/README.md)
```

## Process

- **Specs**: every feature gets `specs/NNN-slug/spec.md` (from `specs/TEMPLATE.md`), kept current;
  index in `specs/README.md`. Research/thinking-out-loud goes to gitignored `design/NNN-topic/`.
- **CI** (`.github/workflows/ci.yml`): lint (clang-format 11.0.1 + yamllint) + Linux build/test on
  PRs, macOS on merges. Builds cache via ccache (`EXT_FLAGS` compiler launchers + actions/cache) and
  a vcpkg binary cache. `distribution.yml` runs the community-extensions matrix on main/tags.
- Do not commit `design/`; do not push without being asked.

## Reference repos (local)

- `~/projects/hugr-lab/mssql-extension` — the mssql side: `mssql_exec`/`mssql_scan` (transaction
  pinning), type codecs (`src/codec/`), DML/PK constraints.
- `~/projects/duckdb/ducklake` — upstream ducklake clone; the manager surface is
  `src/include/storage/ducklake_metadata_manager.hpp`, the perf references are
  `src/metadata_manager/{postgres,quack}_metadata_manager.cpp`.
- `~/projects/hugr-lab/duckdb-acl` — sibling extension repo whose build/CI conventions this repo
  follows.
