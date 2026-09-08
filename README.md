# mssql_ducklake

A [DuckDB](https://duckdb.org) **bridge extension** that lets [DuckLake](https://ducklake.select)
keep its metadata catalog in **Microsoft SQL Server**, through the
[mssql extension](https://github.com/hugr-lab/mssql-extension):

```sql
INSTALL ducklake FROM hugr;  INSTALL mssql FROM hugr;  INSTALL mssql_ducklake FROM hugr;
LOAD mssql_ducklake;   -- autoloads both sides, gates versions, registers the metadata manager
ATTACH 'ducklake:mssql://user:pass@host?database=lake_meta' AS lake (DATA_PATH 's3://…');
```

Neither side changes: `mssql` works standalone exactly as before, `ducklake` is untouched upstream
source. The bridge contributes a `MSSQLMetadataManager` (T-SQL dialect, proper keys and filtered
indexes, inlining support) to DuckLake's metadata-manager registry, and refuses to load unless both
`ducklake` and `mssql` are available — there is nothing for it to do alone.

**Status: early scaffold.** The deps gate and build/CI are in place; the metadata manager itself is
being implemented — see [`specs/`](specs/README.md) for the design.

## Versions

Release-pinned across the stack: duckdb `v1.5.5`, ducklake `v1.5-variegata`, mssql `v0.2.4`. The
loadable bridge pairs with a ducklake artifact that exports its registration symbol (distributed
from the hugr extension repository; static builds need no export).

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
