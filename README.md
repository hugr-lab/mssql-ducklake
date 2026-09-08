# mssql_ducklake

**[DuckLake](https://ducklake.select) on Microsoft SQL Server, batteries included** — a
[DuckDB](https://duckdb.org) extension that embeds DuckLake and adds a SQL Server metadata-catalog
manager, speaking to the server through the
[mssql extension](https://github.com/hugr-lab/mssql-extension):

```sql
INSTALL mssql FROM community;
INSTALL mssql_ducklake FROM community;   -- once published
LOAD mssql_ducklake;
ATTACH 'ducklake:mssql:Server=host,1433;Database=lake_meta;User Id=…;Password=…' AS lake (DATA_PATH 's3://…');
```

**mssql v0.2.5 or newer is required** — older versions answer DuckDB's `main` when DuckLake asks the
catalog for its default schema, and the ATTACH above fails with `Schema 'main' not found in MSSQL
database`. Until [community-extensions#2676](https://github.com/duckdb/community-extensions/pull/2676)
lands, `INSTALL mssql FROM community` still gives v0.2.4; with that version, add
`METADATA_SCHEMA 'dbo'` to the ATTACH.

The extension carries the complete, unmodified DuckLake source at a pinned release and registers a
`MSSQLMetadataManager` beside the built-in postgres/sqlite ones — so everything DuckLake does
(snapshots, time travel, inlining, maintenance functions, `ducklake:postgres:` catalogs too) works
exactly as stock, plus SQL Server as a metadata catalog.

> **Mutually exclusive with the stock `ducklake` extension.** Load one or the other, never both —
> the embedded copy provides the same functions and the same `ducklake:` ATTACH prefix, and
> `LOAD mssql_ducklake` refuses with a clear message when stock ducklake is already loaded. Load
> `mssql_ducklake` **before** the first `ATTACH 'ducklake:…'`, otherwise DuckDB autoloads the stock
> extension for that prefix.

**Status: experimental.** The embedded-ducklake scaffold, gates and CI are in place. With mssql
v0.2.5 ([spec 003](specs/003-mssql-extension-v0.2.5/spec.md)) an ATTACH initializes and re-opens a
DuckLake catalog in SQL Server, and through DuckLake's own generic manager a table's **first** write
commits — `CREATE TABLE`, then one insert, inlined or file-backed. Every later write to that table
fails: the commit batch updates the table's statistics row, that UPDATE takes DuckDB's DML path, and
the mssql extension requires a primary key for it. Removing that path is the first job of the SQL
Server metadata manager (its `Execute` passthrough sends the batch as raw T-SQL), which is being
implemented — see [`specs/`](specs/README.md). If this
extension finds users, the manager is intended to be contributed upstream to DuckLake (the postgres
metadata manager is the in-tree precedent), after which this extension becomes unnecessary.

## Versions

One release line, bumped together: duckdb `v1.5.5`, ducklake `v1.5-variegata` (embedded), mssql
`v0.2.5`. The embedded ducklake bumps on this repository's schedule; the pin is always named in the
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

### Integration tests (SQL Server in docker)

`test/sql/integration/` runs against a real SQL Server holding the DuckLake catalog. The
environment is `docker/docker-compose.yml` (image pinned like the rest of the stack); every
container and volume is named `mssql-ducklake-*` and the port is 7433, so it lives beside other
SQL Servers on the same machine:

```bash
cp -n .env.example .env     # once; the port, the sa password and the database name live here
make docker-up              # start, wait for healthy, create the catalog database
make test-integration       # the server-backed suite; `make test` skips it (require-env)
make docker-down            # stop, keeping the data volume
docker compose -f docker/docker-compose.yml down -v     # ...and drop it
```

The tests reset that database (every `ducklake%` table) on each run; the init plants a marker table
in it, and a run refuses to reset a database without the marker. CI's Linux job uses the same
compose file and targets.

### Connection strings

The catalog attaches as `ducklake:mssql:` followed by an ADO-style connection string, as above:
DuckDB strips the `mssql:` prefix and hands the rest to the mssql extension. The URI form needs the
type spelled out, because DuckDB deliberately does not treat `mssql://` as a prefix:

```sql
ATTACH 'ducklake:mssql://user:pass@host:1433/lake_meta' AS lake (DATA_PATH 's3://…', META_TYPE 'mssql');
```

The catalog tables land in `dbo`, which is what mssql v0.2.5 answers as the catalog's default schema
— a constant, not the login's own default. A login whose default schema is something else, or which
may only write elsewhere, still needs `METADATA_SCHEMA` naming that schema.

## License

MIT — see [LICENSE](LICENSE). Embeds [DuckLake](https://github.com/duckdb/ducklake) (MIT) as a git
submodule at an unmodified pinned release.
