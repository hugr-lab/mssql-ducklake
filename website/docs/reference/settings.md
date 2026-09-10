---
title: Settings
sidebar_position: 1
---

# Settings and functions

### DuckDB settings

| setting | type | default | meaning |
| --- | --- | --- | --- |
| `mssql_ducklake_forced_parameterization` | BOOLEAN | `true` | Whether shaping a catalog sets `PARAMETERIZATION FORCED` on its database ([Shaping](../catalog/shaping.md#forced-parameterization)). Read when the catalog is shaped — at creation, or at the first attach with a build whose shape version is newer — not on every attach. `false` leaves the database's setting as it is. |

DuckLake's own settings — `ducklake_max_retry_count`, `ducklake_retry_wait_ms`,
`ducklake_retry_backoff`, `ducklake_default_data_inlining_row_limit` — apply unchanged, as do the
mssql extension's connection settings (`mssql_connection_limit`, timeouts, …) for the catalog's
connections; see the [mssql extension's reference](https://hugr-lab.github.io/mssql-extension/reference/settings/).

### Attach options

`DATA_PATH`, `METADATA_SCHEMA`, `META_TYPE` and DuckLake's own — on the [attach page](../catalog/attach.md#options).

### Functions

| function | returns |
| --- | --- |
| `mssql_ducklake_version()` | the extension's version string |

Everything else is DuckLake's: `ducklake_snapshots()`, `ducklake_table_info()`,
`ducklake_table_changes()`, `ducklake_flush_inlined_data()`, `ducklake_expire_snapshots()`,
`ducklake_merge_adjacent_files()`, `ducklake_cleanup_old_files()`, and the rest of the
[DuckLake surface](https://ducklake.select/docs/stable/duckdb/introduction).

### Environment switches

Read once per process, for measurement and for tests that must be shown to fail; none is needed
for ordinary use.

| variable | effect |
| --- | --- |
| `MSSQL_DUCKLAKE_SERVER_COMMIT=1` | turns on the [server-side commit](../writing.md#the-server-side-commit-experimental) for commits that add data files and nothing else |
| `MSSQL_DUCKLAKE_SERVER_COMMIT_MIN_FILES=<n>` | the commit size (data files) from which the server-side path is used; default 16, the measured crossover on a loopback link |
| `MSSQL_DUCKLAKE_SERVER_COMMIT_SKIP_FETCH=1` | with the server-side commit, lets the server allocate the snapshot instead of the client fetching it first |
| `MSSQL_DUCKLAKE_NO_CONFLICT_REWRITE=1` | runs DuckLake's conflict check in its original two-read form instead of the one-statement T-SQL form — exists so the concurrency regression test can be shown to fail |
