---
title: Writing
sidebar_position: 3
---

# Writing

DuckLake's write path is DuckLake's: `INSERT`, `UPDATE`, `DELETE`, `MERGE`, `ALTER TABLE`,
`CREATE TABLE AS`, partitioning, and the maintenance functions (`ducklake_flush_inlined_data`,
`ducklake_expire_snapshots`, `ducklake_merge_adjacent_files`, `ducklake_cleanup_old_files`) all
work against a SQL Server catalog. This page is what is specific to it.

### How a commit reaches the server

DuckLake assembles a commit as one batch of DuckDB SQL — inserts into its catalog tables, the
stats update, the deletes of a drop or an expiry — and hands it to the manager. The manager knows
every statement that batch can carry: a closed list captured off every write DuckLake can make
(the repository's design notes), each recognised **exactly** and sent as the manager's own T-SQL,
contiguous runs in one call on the transaction's connection. Nothing is translated: a statement
matches to the character, with its literals where the template says literals go, or it is left
to DuckDB's own path through the mssql extension's DML operators. Two statements are: the
`INSERT` of a user's inlined rows — their values are the user's, in every DuckDB literal form — and
anything a future DuckLake writes that the list does not have, which then runs correctly, slower.
The primary keys the [shaping](./catalog/shaping.md) adds are what make that fallback path
work at all; the T-SQL path is what makes a commit one round trip instead of ~19.

A commit's data-file rows, statistics and partition values go through DuckLake's appender by
default (`MSSQL_DUCKLAKE_NO_APPENDER=1` puts them in the batch instead;
[Performance](./performance.md) has both measured).

### Inlining and types

Inserts below the inlining limit (`DATA_INLINING_ROW_LIMIT`, 10 rows by default) are stored in the
catalog rather than in a data file, in a `ducklake_inlined_data_<table>_<schema version>` table
the manager creates with T-SQL column types. Types SQL Server holds exactly are stored natively;
the rest are stored as text in DuckLake's canonical form — lossless, but a filter on such a column
is evaluated by DuckDB after the rows come back, not pushed to the server.

| stored natively | stored as text (`VARCHAR(MAX)`) |
| --- | --- |
| `BOOLEAN`, the signed and unsigned integers up to `BIGINT`, `DECIMAL(p, s)`, `VARCHAR`, `BLOB` | `FLOAT`, `DOUBLE` — SQL Server has no NaN or infinities, so they could not round-trip |
| `DATE`, `TIME`, `TIMESTAMP`, `TIMESTAMP_MS`, `TIMESTAMP_S`, `TIMESTAMP WITH TIME ZONE`, `UUID` | `TIMESTAMP_NS`, `TIME_NS` — `DATETIME2` resolves to 100 ns |
| | `UBIGINT`, `HUGEINT`, `UHUGEINT` — wider than `DECIMAL(38, 0)` |
| | `INTERVAL`, `TIME WITH TIME ZONE`, `BIT`, `ENUM`, `GEOMETRY`; `STRUCT`, `LIST`, `MAP`, `ARRAY`, `UNION` (nested types are text in every DuckLake backend) |

`VARIANT` columns are never inlined — DuckLake would fail mid-commit on one it cannot store
natively — so a table with a `VARIANT` column writes data files from its first row.

### Concurrent writers

Several DuckDB processes may write to one catalog. DuckLake's commit protocol allocates snapshot
ids optimistically and retries a commit that lost the race; on SQL Server the conflict check a
retry makes is one T-SQL statement, so it sees one consistent state — the DuckDB-SQL form read
`ducklake_snapshot` twice on the same connection and could see a commit land between the two
reads. Measured with four writers over fourteen rounds, no writer loses a commit; the concurrency
test in CI (`make test-concurrent`) pins it.

The retry loop is DuckLake's, with DuckLake's settings (`ducklake_max_retry_count`,
`ducklake_retry_wait_ms`, `ducklake_retry_backoff`). A systemic alternative — `SNAPSHOT`
isolation on the catalog's connection — is proposed for the mssql extension
([hugr-lab/mssql-extension#331](https://github.com/hugr-lab/mssql-extension/issues/331)).

### The server-side commit (experimental)

A second commit path stages the commit's rows in `#temp` tables and applies them in one T-SQL
batch on the server, with the retry loop server-side. It is correct for the commits it accepts —
data files and nothing else — and off by default because below about sixteen files per commit the
staging costs more than the loop it replaces. `MSSQL_DUCKLAKE_SERVER_COMMIT=1` turns it on for
measurement ([Settings](./reference/settings.md#environment-switches)).
