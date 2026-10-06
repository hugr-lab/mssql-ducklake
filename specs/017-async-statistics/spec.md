# Spec 017: asynchronous statistics on the catalog's database

- **Status**: implemented
- **Date**: 2026-10-06
- **Author**: vgsml, with Claude

## Summary

The shaping sets `AUTO_UPDATE_STATISTICS_ASYNC ON` on the catalog's database, next to
`PARAMETERIZATION FORCED` (specs/012) and the same way: best-effort, with an opt-out
(`mssql_ducklake_async_statistics`), applied when the catalog is shaped. A query that finds a
table's statistics stale then runs on the ones it has, and the update happens beside it.

## Problem

The scale bench's `deep_read_filtered` - one filtered read of a table after 1000 file-backed
commits into it - went from 0.19 s on the DuckDB 1.5.6 line to 1.13 s on 2.0 (postgres: 0.02 s).
Reproduced on its own (`attach_probe.py --deep-read 1000 --deep-build`): the first read after the
writes took 1234 ms, of which the server-side file list was 1190 ms; the second read 26 ms, the
same statement 9 ms. The server's statistics show why: six statistics of
`ducklake_file_column_stats` (298,500 rows) and three of `ducklake_data_file` were updated at the
moment of that first read. By default SQL Server updates stale statistics synchronously - the query
that crossed the modification threshold waits for the recompute. A catalog crosses it all the time:
every commit adds a data file row and a statistics row per column. postgres' `ANALYZE` is a
background job, so its reads never wait for it.

## Design

`ApplyDatabaseOptions` (was `ApplyForcedParameterization`) applies two options through one
`ApplyDatabaseOption(setting, option, without_it)`: each read from its own setting, each on a
connection of its own outside the transaction, each a warning in `duckdb_logs()` naming the
statement when the server refuses it. `SHAPE_VERSION` 6 → 7, so a catalog shaped by an earlier build
gets the option at its next attach.

## Testing

`attach_mssql.test` puts the database back to synchronous statistics before the first attach and
asserts the attach set it; the opt-out block asserts a re-shaping with the setting false leaves it
off; the opt-in block that it comes back. Measured: the first read after 1000 commits 1234 → 90 ms.

## Alternatives considered

- **Updating the statistics ourselves** after a commit, or on a schedule: work the server does
  better and already offers.
- **Turning automatic statistics off**: plans on statistics that never change would degrade as
  the catalog grows.
