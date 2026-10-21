# mssql_ducklake

[![CI](https://github.com/hugr-lab/mssql-ducklake/actions/workflows/ci.yml/badge.svg)](https://github.com/hugr-lab/mssql-ducklake/actions/workflows/ci.yml)
[![DuckDB](https://img.shields.io/static/v1?label=duckdb&message=v2.0.0&color=blue)](https://github.com/duckdb/duckdb/releases/tag/v2.0.0)
[![Latest release](https://img.shields.io/github/v/release/hugr-lab/mssql-ducklake?label=release&color=blue)](https://github.com/hugr-lab/mssql-ducklake/releases/latest)
[![Docs](https://img.shields.io/badge/docs-hugr--lab.com%2Fdocs%2Fmssql--ducklake-1c7d78)](https://hugr-lab.com/docs/mssql-ducklake/)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**[DuckLake](https://ducklake.select) on Microsoft SQL Server** — a [DuckDB](https://duckdb.org)
extension that embeds DuckLake and keeps its catalog in SQL Server or Azure SQL, speaking to the
server through the [mssql extension](https://github.com/hugr-lab/mssql-extension). Everything
DuckLake does works as stock — snapshots, time travel, inlining, maintenance, `ducklake:postgres:`
catalogs too — plus SQL Server as a metadata catalog.

**Documentation: [hugr-lab.com/docs/mssql-ducklake](https://hugr-lab.com/docs/mssql-ducklake/)**
— getting started, what the catalog needs from the server and what the extension does to it,
performance against the PostgreSQL backend, settings, limitations, troubleshooting.

```sql
INSTALL mssql FROM community;
INSTALL mssql_ducklake FROM community;
LOAD mssql_ducklake;
ATTACH 'ducklake:mssql:Server=host,1433;Database=lake_meta;User Id=…;Password=…' AS lake (DATA_PATH 's3://…');
```

> **Mutually exclusive with the stock `ducklake` extension.** Load one or the other, never both —
> the embedded copy provides the same functions and the same `ducklake:` ATTACH prefix, and
> `LOAD mssql_ducklake` refuses with a clear message when stock ducklake is already loaded. Load
> `mssql_ducklake` **before** the first `ATTACH 'ducklake:…'`, otherwise DuckDB autoloads the stock
> extension for that prefix.

**Status: experimental.** Installable with `INSTALL mssql_ducklake FROM community`:

- v0.2.0 for DuckDB v2.0.0, with DuckLake catalog format 1.1;
- v0.1.2 for DuckDB v1.5.6.

In place:

- the full DuckLake surface on a SQL Server catalog;
- DDL, inlined and file-backed writes, `UPDATE`, `DELETE`, `MERGE`;
- concurrent writers;
- a server-backed integration suite and a concurrency test in CI.

A benchmark against the PostgreSQL backend on equal Azure VMs took 0.63× of its time, at 500
tables. The [limitations](https://hugr-lab.com/docs/mssql-ducklake/reference/limitations/) page
says what is not there yet. The design and every measurement behind it are in
[`specs/`](specs/README.md). If the extension finds users, the manager is meant to be contributed
upstream to DuckLake (the PostgreSQL manager is the in-tree precedent).

## Versions

One release line, bumped together:

- duckdb `v2.0.0`;
- ducklake `main` at a named commit, embedded;
- mssql `v0.3.0` or newer at runtime.

The embedded ducklake bumps on this repository's schedule, and every release names its pin.

The DuckDB v1.5.6 line lives on in the `v1.5` branch: v0.1.2, ducklake `v1.5-variegata`, mssql
v0.2.5 or newer.

mssql v0.3.0 verifies the server's TLS certificate by default. A server on a self-signed
certificate needs `TrustServerCertificate=yes` in the connection string.

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
described on the [development page](https://hugr-lab.com/docs/mssql-ducklake/development/).

## License

MIT — see [LICENSE](LICENSE). Embeds [DuckLake](https://github.com/duckdb/ducklake) (MIT) as a git
submodule at an unmodified pinned release.
