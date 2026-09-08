# mssql_ducklake

A [DuckDB](https://duckdb.org) **bridge extension** that lets [DuckLake](https://ducklake.select)
keep its metadata catalog in **Microsoft SQL Server**, through the
[mssql extension](https://github.com/hugr-lab/mssql-extension):

```sql
INSTALL ducklake;                       -- official extension repository
INSTALL mssql FROM community;
INSTALL mssql_ducklake FROM community;  -- once published
LOAD mssql_ducklake;  -- autoloads both sides, gates versions, registers the metadata manager
ATTACH 'ducklake:mssql://user:pass@host?database=lake_meta' AS lake (DATA_PATH 's3://…');
```

Neither side changes: `mssql` works standalone exactly as before, `ducklake` is untouched upstream
source. The bridge contributes a `MSSQLMetadataManager` (T-SQL dialect, proper keys and filtered
indexes, inlining support) to DuckLake's metadata-manager registry, and refuses to load unless both
`ducklake` and `mssql` are available — there is nothing for it to do alone.

**Status: early scaffold.** The deps gate and build/CI are in place; the metadata manager itself is
being implemented — see [`specs/`](specs/README.md) for the design.

## Versions & distribution

Release-pinned across the stack: duckdb `v1.5.5`, ducklake `v1.5-variegata`, mssql `v0.2.4` — one
line, bumped together.

Distribution is through the **community extensions repository** for now: DuckDB v1.5.5 has no
external extension repositories (those arrive with DuckDB 2.0 — `CREATE EXTENSION REPOSITORY` with
per-repository keys). The loadable bridge pairs with a ducklake build that exports its registration
symbol (untouched upstream plus one linker line), shipped as a release artifact of this repository
and loaded explicitly; with a stock ducklake the bridge refuses at its gate and DuckLake behaves
exactly as it would without the bridge. A static (bundled) build needs no export. With DuckDB 2.0
the hugr extension repository serves all three and `INSTALL … FROM hugr` becomes the bootstrap.

## Building

```bash
git clone --recurse-submodules https://github.com/hugr-lab/mssql-ducklake.git
cd mssql-ducklake
make vcpkg-setup      # once
GEN=ninja make
```

Test:

```bash
build/release/test/unittest 'test/sql/*'
scripts/ci/smoke_load.sh
```

## License

MIT — see [LICENSE](LICENSE).
