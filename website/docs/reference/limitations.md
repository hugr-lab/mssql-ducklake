---
title: Limitations
sidebar_position: 2
---

# Known limitations

Stated as limitations, with where each stands. The specs linked are in the
[repository](https://github.com/hugr-lab/mssql-ducklake/tree/main/specs).

- **Experimental, first release in preparation.** v0.1.0 on the DuckDB v1.5.5 line; until it is
  published the extension is built from source.
- **Mutually exclusive with the stock `ducklake` extension**, and it must be loaded before the
  first `ATTACH 'ducklake:…'` ([why](../index.md#how-it-works)).
- **Slower than the PostgreSQL backend on writes**: 1.8x on the whole 1000-table benchmark, 2.7 – 5x
  on commit-heavy phases, because a commit runs through DuckDB's DML operators one statement at a
  time ([Performance](../performance.md)). The server-side commit that closes this is designed
  (specs/005) and shipped in an experimental, off-by-default form for data-file-only commits.
- **Every first touch of a table pays a plan compile** in the mssql extension's per-table metadata
  query when the catalog's database is not forced-parameterized — ~35 ms each. The shaping applies
  the database option; on a database where the login may not, the fix is
  [hugr-lab/mssql-extension#334](https://github.com/hugr-lab/mssql-extension/issues/334), on the
  extension's 2.0 line.
- **Types stored as text in inlined data**: floating point, nanosecond timestamps, 128-bit and
  unsigned 64-bit integers, intervals, enums, geometry and nested types are stored as text in the
  inlined-data tables — lossless, not filterable server-side ([Writing](../writing.md#inlining-and-types)).
  `VARIANT` columns are never inlined.
- **A retried commit can leave an empty, unregistered `ducklake_inlined_data_*` table behind**;
  harmless, and `ducklake_cleanup_old_files` does not remove it. A flush leaves the emptied inlined tables in place — DuckLake's behaviour on every
  backend.
- **`mssql_scan()` inside a transaction** must be the sole source of its query with mssql v0.2.5:
  a plan mixing it with a catalog scan fails depending on execution order. The manager follows the
  rule; a user query joining `mssql_scan()` against the lake inside a transaction may hit it. Fixed
  in the mssql extension's 2.0 line (#314).
- **Fabric Warehouse and Synapse dedicated pools cannot hold a catalog**
  ([Requirements](../catalog/requirements.md)).
- **No WebAssembly build** — the mssql extension is raw TDS sockets.
- **Concurrent writers rely on DuckLake's retry loop.** No writer loses a commit in the measured
  runs; the systemic alternative, `SNAPSHOT` isolation on the catalog connection, is proposed for
  the mssql extension ([#331](https://github.com/hugr-lab/mssql-extension/issues/331)).
