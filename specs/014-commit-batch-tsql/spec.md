# Spec 014: the commit batch as our T-SQL, statement by known statement

- **Status**: implemented
- **Date**: 2026-09-10
- **Author**: VGSML

## Summary

A commit reaches SQL Server as DuckDB SQL, statement by statement, through the mssql extension's
DML operators — ~19 round trips, and a full scan of `ducklake_table_column_stats` for its stats
update — while the PostgreSQL manager hands the whole batch to the server in one call. That is
the write side's remaining gap: `first_commits` 5x, `partitioned_commits` 5x, `second_commits`
2.7x, `flush_inlined` 1.6x against postgres on the 1000-table benchmark (specs/012).

This spec closes it the way specs/007 and 008 did on the read side — **not** by translating
DuckDB SQL into T-SQL, but by recognising each statement DuckLake writes into the batch **exactly**,
from a closed list captured off the real thing, and sending the manager's own T-SQL for it. The
list is the 74 statement shapes a workload of every write DuckLake can make produced
(design 003); all but one — the `INSERT` of a user's inlined rows, whose values are the user's — are
catalog statements over columns whose types the manager knows. Recognised statements go to the
server in one `mssql_exec` call per contiguous run; the inlined-rows `INSERT`, and any statement
the list does not have, go to the base as they are, in order. Nothing is guessed: a statement
either matches a template to the character (with its literals where the template says literals
go) or is not touched.

The seam already exists: specs/006 D5b overrode `Execute` to take one statement — the DDL of the
inlined deletion table — out of the batch and do it the manager's way. This spec is that seam,
grown to the list.

## Problem

`MSSQL_DEBUG=2` on one file-backed insert (specs/009): the commit batch is six `INSERT`/`UPDATE`
statements plus the appender's, each a round trip through DuckDB's planner and the extension's
DML operator; the `UPDATE` of `ducklake_table_stats` is preceded by a scan for its row identity; the
`UPDATE … FROM new_values` of `ducklake_table_column_stats` is a join, so its scan is the whole
table — 41.8K rows on the 1000-table catalog, ~10 ms warm, growing with the catalog. Against it,
DuckLake's reads before the commit are a handful of single-row lookups. PostgreSQL's manager sends
the same batch in one `postgres_execute` and pays ~10 ms for the whole commit; the SQL Server
catalog pays 25–37 ms.

specs/004 chose not to override `Execute` — "no transpiler" — because a general rewrite of
DuckLake's SQL is a maintenance liability that grows with every ducklake bump. That reasoning
stands. What changed is the evidence of what the batch actually contains: a small, closed set of
shapes over catalog tables whose column types are DuckLake's DDL, plus the one statement carrying
user data. A closed list with a guard is what 007 and 008 already are; the batch is a longer list.

## The list

`design/003-server-side-commit/all_writes.sql` is one session of every write DuckLake can make
against a SQL Server catalog — every inlined type, file and inlined inserts, `UPDATE`, `DELETE`,
`MERGE`, `ALTER TABLE` in all forms, rename, views, macros, tags, partitioning, sort keys, `CREATE
TABLE AS`, a multi-statement transaction, flush, expire, merge-adjacent, cleanup, drops — and
`execute_shapes.py` records every `Execute` batch it produced through the `DuckLakeMetadata` log:
57 batches, 247 statements, 74 shapes. Grouped by what the rewrite has to do:

| family | shapes | statements | what they carry |
| --- | ---: | ---: | --- |
| `INSERT INTO {METADATA_CATALOG}.<catalog table> VALUES (…)[, (…)]` | 27 | 110 | numbers, `'strings'`, `NULL`, `true`/`false`, `NOW()` — into columns whose types are DuckLake's DDL |
| `UPDATE {METADATA_CATALOG}.<t> SET col = n[, …] WHERE <numeric predicates>` | 14 | 33 | numbers, `IS NULL`, `IN (…)` |
| `DELETE FROM {METADATA_CATALOG}.<t> [alias] [WHERE …]` | 15 | 34 | numbers, `IN (…)`, `NOT EXISTS (SELECT … FROM {METADATA_CATALOG}.<t2> …)` |
| `WITH <cte>(…) AS (VALUES …) UPDATE {METADATA_CATALOG}.<t> SET … FROM <cte> WHERE …` | 4 | 13 | the stats refresh, inlined-row deletes, dropped columns, overwritten tags; `CAST(x AS BOOLEAN)` |
| `DROP TABLE IF EXISTS {METADATA_CATALOG}.<inlined table>` | 1 | 5 | — |
| `CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.ducklake_inlined_delete_<n>(…)` | 1 | 1 | specs/006 D5b, already the manager's |
| `INSERT INTO {METADATA_CATALOG}.ducklake_inlined_data_<t>_<v> VALUES (…)` | 12 | 51 | **the user's rows**: every DuckDB literal form, `CAST('…' AS DATE)`, `CAST('\xDE\xAD' AS BLOB)`, unicode |

The last family stays with DuckDB. Its values are rendered by DuckDB for DuckDB — the grammar of
every type's literal — and re-rendering them for SQL Server is the transpiler this spec does not
build. The cost is one boundary per inlined insert: the recognised statements before it go in one
call, the `INSERT` runs through the base, the statements after it go in one call.

## Design

**D1 — exact templates, a whitelist for values.** Each family is recognised by its fixed text
(the base's generator, verbatim, with `{METADATA_CATALOG}` still unsubstituted — `Execute` sees the
batch before the base's placeholder pass, which is why this overload and not `Query`) and its
variable parts are read by a tokenizer that knows five literal kinds: integer, decimal, `'string'`
with `''` escaping, `NULL`, `true`/`false` — and `NOW()`. A tuple with anything else in it, a
column count that is not the table's, a table name that is not on the catalog list, a predicate
with a function call or a string where the family has none: the statement is not recognised and
goes to the base unchanged. Recognition is total or it is nothing.

**D2 — the rewrite, per family.** With `{METADATA_CATALOG}.x` → `<schema>.x` everywhere:

- *INSERT into a catalog table*: column by column, from a static table of every catalog table's
  column types (DuckLake's DDL, `ducklake_metadata_manager.cpp:194-221`, plus the two inlined
  bookkeeping tables): `BIGINT` verbatim; `VARCHAR` → `N'…'`, so that unicode arrives as UTF-16
  and lands in the UTF-8 column without passing through the database's legacy code page;
  `BOOLEAN` → `1`/`0`; `TIMESTAMPTZ` → `NOW()` becomes `SYSDATETIMEOFFSET()`; `UUID` → the string
  literal, which SQL Server converts implicitly. `NULL` verbatim.
- *UPDATE and DELETE with numeric predicates*: the text is T-SQL already; the schema prefix is
  swapped and the statement is sent as it is. The alias form `DELETE FROM x tbl WHERE NOT EXISTS
  (…)` is not — SQL Server refuses an alias after the target — and becomes `DELETE tbl FROM x tbl
  WHERE …`, which is (verified on the server).
- *the CTE updates*: `WITH c(…) AS (SELECT * FROM (VALUES …) AS v(…)) UPDATE x SET … FROM c WHERE
  …` — a `VALUES` list needs the `SELECT` wrapper in T-SQL, `CAST(… AS BOOLEAN)` becomes `CAST(… AS
  BIT)`, the values follow the INSERT rules. The target not appearing in `FROM` is valid T-SQL and
  does what DuckDB's form does (verified).
- *DROP TABLE IF EXISTS*: T-SQL since 2016; the manager also refreshes the extension's cache entry.

**D3 — runs, in order.** The batch is split on statement boundaries (a `;` outside a string);
consecutive recognised statements form one T-SQL batch, sent through `mssql_exec` on the
transaction's pinned connection — the same connection the base's statements use, so a mixed batch
stays one transaction. An unrecognised statement is handed to the base's `Execute` alone, with the
runs before and after it sent as they were formed. `Execute`'s result is the last run's, or the
base's if it failed; an error in a T-SQL run comes back in the `QueryResult` the way the base's
does, so the commit loop's retry (`RetryOnError` matches "primary key" — SQL Server's
`Violation of PRIMARY KEY constraint` does) and rollback are untouched. No `XACT_ABORT`: a failed
statement fails the run, the transaction stays open for DuckLake to roll back, and the statements
after it in the run are discarded with it.

**D3b — what the implementation found.** Two things the capture did not show until the T-SQL
ran. `key` — a column of `ducklake_metadata`, `ducklake_tag` and `ducklake_column_tag`, and a CTE
column of the overwritten-tags update — is a reserved word in T-SQL and bare in DuckDB; the rewrite
brackets it wherever a name is emitted, as the shaping's primary keys already did. And the strict
guard earned its place on its first run: the all-writes workload failed naming the overwritten-tags
statement (`Incorrect syntax near the keyword 'key'`) rather than a commit failing somewhere later.
The D5b DDL is the sixth family, folded into the same seam and the same file. And SQL Server
takes at most a thousand rows in one `VALUES` list (error 10738): a commit of two hundred files
writes eight thousand statistics rows in one `INSERT`, which the appender-off bench found; the
rewrite emits the statement per thousand rows — still one round trip, since the run is one batch —
and the CTE updates the same way, each row of theirs being an independent update.

The identity check: the all-writes workload run through the rewrite and through the base path
(`MSSQL_DUCKLAKE_NO_BATCH_REWRITE=1`) leaves the same catalog — the row count of every
`ducklake_*` table equal — and `write_shapes.test` passes on both.

**D3c — the writes DuckLake sends through `Query`.** Not every write is in the commit batch: the
expiry and the cleanup `DELETE` from a dozen tables through `Query`, one statement at a time, and
the flush `DELETE`s the rows it moved to files. CI found what the base path costs there — on
linux_amd64 the expiry's `DELETE FROM ducklake_tag … NOT EXISTS (…)` through the extension's DML
operator failed with `Invalid unicode (byte sequence mismatch) detected in value construction`
(the composite-key row identity of a table keyed on a `VARCHAR(200)` column), where the same
statement passed on macOS and, in isolation, on the community 0.2.5 under emulation. The
statements are the batch's own families, so `Query` hands a recognised `UPDATE` or `DELETE` to the
same rewrite (`TryRewriteWrite`): one T-SQL statement, one round trip, no scan of the table through
the DML operator — and the base path is a fallback there too. The observation stays recorded for
the extension; it is not reproduced.

**D4 — the guard.** `MSSQL_DUCKLAKE_STRICT_BATCH=1` makes an unrecognised statement over a
`ducklake_` table — anything but the inlined-rows `INSERT` — an error instead of a fallback. The
integration suite and the write-shapes workload run with it on, so a ducklake bump that adds a
shape fails the suite naming the statement, and the closed list is re-audited the way 007's and
D5b's templates are. Off by default: a user's catalog keeps working through the base path, slower.

**D5 — the appender stays on.** With `SupportsAppender` true (specs/006) a commit's data-file
rows, statistics and partition values bypass the batch and go through DuckDB's appender — one DML
round trip per table, its rows in `VALUES` chunks of a thousand. Off
(`MSSQL_DUCKLAKE_NO_APPENDER=1`), they are `INSERT … VALUES` statements in the batch and this
spec's rewrite sends them with everything else in one call. Measured both ways, the mssql arm of
the 1000-table bench: off is slower on every phase it touches — `partitioned_commits` 33.0 against
30.6, `flush_inlined` 262 against 239, `partitioned_merge_adjacent` 27.4 against 18.2, total 612
against 578 — the round trips it saves cost less than the literal statements it compiles. And the
mssql extension's *INSERT via BCP* (its spec 062) ships with its first release on the DuckDB 2.0
line: the appender path becomes bulk loads with no code here, which is the cure for the phase
that stays at 5x (`partitioned_commits`, 25 files a commit). A staging branch of the rewrite for
large inserts would live exactly until that bump; not built.

**D6 — what this does to specs/005.** The `#temp` staging path stays as it is — off by default,
for commits of many data files where bulk loads beat literals, and still passing its suite
(`make test-integration-fast-path`, strict). Its threshold of sixteen files (005 D7) was measured
against the DML path this spec replaces, so it is optimistic now; not re-measured here — with the
appender becoming bulk loads on the 2.0 line (D5) the path's future is a bump away.

## Enforcement & security

- Values reach the server as literals the manager rendered from literals DuckLake rendered — no
  user string is interpreted, and `''` escaping is the same in both dialects. `N'…'` is the only
  change to a string.
- The T-SQL runs on the pinned connection inside DuckLake's transaction; nothing is committed that
  DuckLake did not commit.
- The strict switch exists for the suite; a catalog never fails because of it in production.

## Testing

- `test/sql/integration/write_shapes.test`: the all-writes workload as a sqllogictest, run with
  the strict switch on — every family, every literal kind, unicode, `NULL`, booleans, `NOW()`; the
  results asserted from the lake (the quote and the unicode back from the inlined table and from
  the column statistics, time travel, the view, the macro, partition pruning, the flush and the
  expiry); its catalog lives in a schema of its own. `MSSQL_DUCKLAKE_NO_BATCH_REWRITE=1` runs the
  same file through the base path — the two must agree, and the row counts of every catalog table
  at the end do.
- The existing integration suite on both paths (strict on; `MSSQL_DUCKLAKE_SERVER_COMMIT=1`), `make
  test-concurrent` — the retry path through a T-SQL primary-key error.
- `make bench-scale --tables 1000` before and after, both arms in one run, the appender on:

  | phase | before (012) | **after** | postgres | ratio |
  | --- | ---: | ---: | ---: | ---: |
  | `first_commits` | 29.1 | **16.7** | 5.8 | 2.9x |
  | `second_commits` | 25.5 | **11.4** | 9.0 | **1.27x** |
  | `deep_history` (1000 commits, one table) | 77.7 | **35.3** | 30.5 | **1.16x** |
  | `merge_adjacent` | 34.2 | **3.4** | 22.4 | **0.15x** |
  | `flush_inlined` | 248 | 240 | 150 | 1.6x |
  | `partitioned_commits` | 31.9 | 30.7 | 6.1 | 5.0x |
  | `partitioned_merge_adjacent` | 20.8 | 18.9 | 12.5 | 1.5x |
  | `evolution` (100 schema changes) | 41.0 | 39.1 | 25.9 | 1.5x |
  | `filtered_read` | 0.45 | 0.37 | 0.18 | 2.1x |
  | **total** | **698** | **577** | **375** | **1.54x** |

  The commit-bound phases are at or near parity: a file-backed commit costs 11.4 ms against
  postgres's 9.0, a thousand commits into one table 35 s against 30, and the compaction that
  rewrites file metadata for many files is six times faster than postgres, whose manager runs that
  batch through a scanner. What stays: the first inlined write (2.9x — the inlined rows' `INSERT`
  through the base path, one boundary in the batch, plus the table's first touch), the partitioned
  commits (the appender's per-file rows, BCP on the 2.0 line), the flush (DuckLake's per-table
  reads and file writes, 1.6x) and the reattach (three logins, specs/005 D8). Against postgres the
  whole benchmark went 2.44x → 1.80x (specs/012) → **1.54x**.

## Alternatives considered

- **A transpiler** — a parser for DuckDB's literal grammar and statement forms, rendering T-SQL.
  Rejected, the same way specs/004 rejected it: it grows with DuckLake and fails silently in the
  gap between what it handles and what it saw. The closed list fails loudly at the guard instead.
- **`FlushChangesServerSide` for every commit** — the quack pattern, the manager building the
  commit from the transaction's change sets rather than from text. It would re-implement DuckLake's
  commit protocol for every change shape; specs/005 did it for data files alone and it was the
  largest piece of code in the manager. The text DuckLake already assembled is the protocol.
- **Rewriting only the column-stats `UPDATE`** — the one statement with a catalog-sized scan.
  Cheaper, but the round trips stay; the bench says both matter.
- **Rendering the inlined rows too**, from the inlined table's known column types. Possible for
  the types the manager stores natively, and the values are the only place a user's text and bytes
  travel; deferred until the rest is measured.

## Follow-ups

- After the 2.0 bump the extension can call `sp_executesql`; nothing here depends on it.
- specs/005's threshold, re-measured against this batch if the staging path is kept past the 2.0 bump.
