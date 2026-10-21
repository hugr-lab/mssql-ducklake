---
title: Limitations
sidebar_position: 2
---

# Known limitations

Stated as limitations, with where each stands. The specs linked are in the
[repository](https://github.com/hugr-lab/mssql-ducklake/tree/main/specs).

- **Experimental.** v0.2.0 for DuckDB v2.0.0, v0.1.2 for v1.5.6, from the community extensions
  repository ([Releases](../releases.md)).
- **Mutually exclusive with the stock `ducklake` extension**, and it must be loaded before the
  first `ATTACH 'ducklake:…'` ([why](../index.md#how-it-works)).
- **Measured on one benchmark.** v0.2.0 is faster than the PostgreSQL backend on it: 0.63× of the
  time on equal Azure VMs, at 500 tables. One workload, one region and one VM size, on pre-release
  builds ([Performance](../performance.md)). On v0.1.2 (DuckDB v1.5.6) the same benchmark ran at
  1.54× of the PostgreSQL time.
- **On DuckDB v1.5.6, every first touch of a table pays a plan compile** in the mssql extension's
  per-table metadata query when the catalog's database is not forced-parameterized: ~35 ms each.
  The shaping applies the database option. mssql v0.3.0, the DuckDB 2.0 build, parameterizes that
  query itself ([hugr-lab/mssql-extension#334](https://github.com/hugr-lab/mssql-extension/issues/334)).
- **Types stored as text in inlined data**: floating point, nanosecond timestamps, 128-bit and
  unsigned 64-bit integers, intervals, enums, geometry and nested types are stored as text in the
  inlined-data tables — lossless, not filterable server-side ([Writing](../writing.md#inlining-and-types)).
  `VARIANT` columns are never inlined.
- **A rolled-back commit can leave an empty `ducklake_inlined_data_*` or `ducklake_inlined_delete_*`
  table behind** — they are created outside the transaction so that the commit can see them;
  harmless, and `ducklake_cleanup_old_files` does not remove them. A flush leaves the emptied inlined tables in place — DuckLake's behaviour on every
  backend.
- **On DuckDB v1.5.6, `mssql_scan()` inside a transaction** must be the sole source of its query,
  which is a rule of mssql v0.2.5. A plan mixing it with a catalog scan fails depending on execution
  order. A user query joining `mssql_scan()` against the lake inside a transaction may hit it.
  mssql v0.3.0, the DuckDB 2.0 build, fixes it (#314).
- **Fabric Warehouse and Synapse dedicated pools cannot hold a catalog**
  ([Requirements](../catalog/requirements.md)).
- **No WebAssembly build** — the mssql extension is raw TDS sockets.
- **Concurrent writers rely on DuckLake's retry loop.** No writer loses a commit in the measured
  runs.
  - mssql v0.3.0 adds a `transaction_isolation` ATTACH option, with `SNAPSHOT` as one choice
    ([#331](https://github.com/hugr-lab/mssql-extension/issues/331)).
  - This extension does not set it for the catalog connection.
