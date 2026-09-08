# mssql_ducklake

**[DuckLake](https://ducklake.select) on Microsoft SQL Server, batteries included** — a
[DuckDB](https://duckdb.org) extension that embeds DuckLake and adds a SQL Server metadata-catalog
manager, speaking to the server through the
[mssql extension](https://github.com/hugr-lab/mssql-extension):

```sql
INSTALL mssql FROM community;
INSTALL mssql_ducklake FROM community;   -- once published
LOAD mssql_ducklake;
ATTACH 'ducklake:mssql://user:pass@host?database=lake_meta' AS lake (DATA_PATH 's3://…');
```

The extension carries the complete, unmodified DuckLake source at a pinned release and registers a
`MSSQLMetadataManager` beside the built-in postgres/sqlite ones — so everything DuckLake does
(snapshots, time travel, inlining, maintenance functions, `ducklake:postgres:` catalogs too) works
exactly as stock, plus SQL Server as a metadata catalog.

> **Mutually exclusive with the stock `ducklake` extension.** Load one or the other, never both —
> the embedded copy provides the same functions and the same `ducklake:` ATTACH prefix, and
> `LOAD mssql_ducklake` refuses with a clear message when stock ducklake is already loaded. Load
> `mssql_ducklake` **before** the first `ATTACH 'ducklake:…'`, otherwise DuckDB autoloads the stock
> extension for that prefix.

**Status: experimental.** The embedded-ducklake scaffold, gates and CI are in place; the SQL Server
metadata manager is being implemented — see [`specs/`](specs/README.md) for the design. If this
extension finds users, the manager is intended to be contributed upstream to DuckLake (the postgres
metadata manager is the in-tree precedent), after which this extension becomes unnecessary.

## Versions

One release line, bumped together: duckdb `v1.5.5`, ducklake `v1.5-variegata` (embedded), mssql
`v0.2.4`. The embedded ducklake bumps on this repository's schedule; the pin is always named in the
release notes.

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

MIT — see [LICENSE](LICENSE). Embeds [DuckLake](https://github.com/duckdb/ducklake) (MIT) as a git
submodule at an unmodified pinned release.
