# mssql_ducklake

**[DuckLake](https://ducklake.select) on Microsoft SQL Server** — a [DuckDB](https://duckdb.org)
extension that embeds DuckLake and keeps its catalog in SQL Server or Azure SQL, speaking to the
server through the [mssql extension](https://github.com/hugr-lab/mssql-extension). Everything
DuckLake does works as stock — snapshots, time travel, inlining, maintenance, `ducklake:postgres:`
catalogs too — plus SQL Server as a metadata catalog.

**Documentation: [hugr-lab.github.io/mssql-ducklake](https://hugr-lab.github.io/mssql-ducklake/)**
— getting started, what the catalog needs from the server and what the extension does to it,
performance against the PostgreSQL backend, settings, limitations, troubleshooting.

```sql
INSTALL mssql FROM community;
INSTALL mssql_ducklake FROM community;   -- from the first release on; until then, build from source
LOAD mssql_ducklake;
ATTACH 'ducklake:mssql:Server=host,1433;Database=lake_meta;User Id=…;Password=…' AS lake (DATA_PATH 's3://…');
```

> **Mutually exclusive with the stock `ducklake` extension.** Load one or the other, never both —
> the embedded copy provides the same functions and the same `ducklake:` ATTACH prefix, and
> `LOAD mssql_ducklake` refuses with a clear message when stock ducklake is already loaded. Load
> `mssql_ducklake` **before** the first `ATTACH 'ducklake:…'`, otherwise DuckDB autoloads the stock
> extension for that prefix.

**Status: experimental.** The first release, v0.1.0 on the DuckDB v1.5.5 line, is in preparation.
In place: the full DuckLake surface on a SQL Server catalog; DDL, inlined and file-backed writes,
`UPDATE`, `DELETE`, `MERGE`; concurrent writers; a server-backed integration suite and a
concurrency test in CI; a 1000-table benchmark against the PostgreSQL backend, at 1.5x of its total
time with commits at parity — the [limitations](https://hugr-lab.github.io/mssql-ducklake/reference/limitations/) page
says what is not there yet. The design and every measurement behind it are in
[`specs/`](specs/README.md). If the extension finds users, the manager is meant to be contributed
upstream to DuckLake (the PostgreSQL manager is the in-tree precedent).

## Versions

One release line, bumped together: duckdb `v1.5.5`, ducklake `v1.5-variegata` (embedded), mssql
`v0.2.5` or newer at runtime. The embedded ducklake bumps on this repository's schedule; the pin is
named in every release. mssql v0.2.5 is the minimum because it answers `dbo` as the catalog's
default schema — older versions answer DuckDB's `main` and the attach fails.

## Building

```bash
git clone --recurse-submodules https://github.com/hugr-lab/mssql-ducklake.git
cd mssql-ducklake
make vcpkg-setup      # once
GEN=ninja make
build/release/test/unittest 'test/sql/*'
scripts/ci/smoke_load.sh
```

The server-backed suite runs against the SQL Server in `docker/` (`make docker-up`,
`make test-integration`, `make test-concurrent`); the benchmarks, the specs and the docs site are
described on the [development page](https://hugr-lab.github.io/mssql-ducklake/development/).

## License

MIT — see [LICENSE](LICENSE). Embeds [DuckLake](https://github.com/duckdb/ducklake) (MIT) as a git
submodule at an unmodified pinned release.
