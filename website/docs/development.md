---
title: Development
sidebar_position: 7
---

# Development

Contributions, bug reports and testing on platforms the suite does not reach (Azure SQL Database,
Managed Instance, Fabric SQL database) are welcome —
[issues](https://github.com/hugr-lab/mssql-ducklake/issues) and pull requests on GitHub.

### Building

```bash
git clone --recurse-submodules https://github.com/hugr-lab/mssql-ducklake.git
cd mssql-ducklake
make vcpkg-setup      # once (or point VCPKG_TOOLCHAIN_PATH at an existing vcpkg)
GEN=ninja make        # release: the duckdb CLI, unittest, the mssql and mssql_ducklake loadables
```

The build produces `build/release/duckdb` with both extensions under `build/release/extension/`;
`build/release/test/unittest 'test/sql/*'` runs the server-free suite and `scripts/ci/smoke_load.sh`
the out-of-tree load and both gates.

### The integration suite

`test/sql/integration/` runs against a real SQL Server holding a DuckLake catalog. The environment
is `docker/docker-compose.yml`, image pinned like the rest of the stack, every container and volume
named `mssql-ducklake-*`, port 7433, so it lives beside other SQL Servers on the same machine:

```bash
cp -n .env.example .env     # port, sa password, database name
make docker-up              # start, wait for healthy, create the catalog database
make test-integration       # the server-backed suite; `make test` skips it
make test-concurrent        # four concurrent writers over alternating rounds
make docker-down
```

The suite resets its database (every `ducklake%` table) on each run; the init plants a marker
table, and a run refuses to reset a database without it. CI runs the same compose file and forbids
a silent skip.

### Benchmarks

`make bench-scale BENCH_SCALE_ARGS='--tables 1000 …'` builds a catalog of the given shape on SQL
Server and on PostgreSQL (`MSSQL_DUCKLAKE_PG_DSN`) and times every phase on both; the
[performance page](./performance.md) is its output. `make metadata-log WORKLOAD=file.sql` lists
every metadata query a workload issued, by shape, cost and path.

### Specs

Each feature has one lightweight spec under [`specs/`](https://github.com/hugr-lab/mssql-ducklake/tree/main/specs)
— the problem, the design with the measurements that decided it, the tests, the alternatives —
written before or alongside the work and kept current. `specs/README.md` is the index; reading
002 (why DuckLake is embedded) and 004 (the manager's first phase) first explains most of the
code.

### This site

`website/` is a Docusaurus site, the same setup as the
[mssql extension's](https://github.com/hugr-lab/mssql-extension/tree/main/website):

```bash
cd website && npm ci && npm start      # live preview
npx docusaurus build                   # what CI and the Pages deploy run; broken links fail it
```

Pull requests that touch `website/` build the site; a push to `main` deploys it. At each release
`npm run docusaurus docs:version <X.Y.Z>` snapshots the docs, which then serve as the default while
the live tree publishes as *Next*. Links between pages are relative (`../catalog/shaping.md`),
never absolute site paths — `scripts/ci/check_docs_links.py` enforces that, because an absolute
link leaves the version the reader is in.
