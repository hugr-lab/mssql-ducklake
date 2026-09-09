# Spec 006: the write path — how rows reach the catalog

- **Status**: implemented
- **Date**: 2026-09-09
- **Author**: VGSML

## Summary

Everything DuckLake writes to its catalog arrives as **statement text**: a commit becomes a batch of
multi-row `INSERT … VALUES`, and a large commit becomes a large batch. This spec asked whether that
is the right shape for SQL Server. The answer turned out to be about neither the protocol nor the
batch size: what makes a large commit expensive is that DuckLake resolves **every data file's path
with two catalog queries of its own**, and the cure — DuckLake's appender, which the other remote
managers decline — costs nothing to adopt.

Along the way the catalog's own storage was corrected: no `NVARCHAR` anywhere, a bounded
`partition_value` so partition pruning can seek, and a shape version so an existing catalog is
actually brought up to date.

## Problem

Writing is the more expensive half at production scale. From the run in specs/005 D8, over a catalog
of 1000 tables with 41 columns each and three tables a thousand commits deep:

| phase | mssql | postgres | ratio |
| --- | ---: | ---: | ---: |
| `flush_inlined` | 328.1s | 150.5s | 2.18x |
| `create_tables` | 169.0s | 98.8s | 1.71x |
| `deep_history` (3000 file-backed commits) | 118.2s | 30.5s | 3.88x |
| `evolution` (100 schema changes) | 46.1s | 30.2s | 1.53x |
| `merge_adjacent` | 44.2s | 22.2s | 1.99x |

The first draft of this spec asserted that DuckLake's appender "does not apply to a remote catalog",
because `SupportsAppender()` is answered `true` by exactly one manager — the base one — and `false`
by postgres, sqlite and quack. That was inference from a pattern, not a measurement, and it was
wrong. Everything below starts from what the code and the server actually do.

## Decisions

### D1 — the appender works, and it is worth taking

`SupportsAppender()` now returns `true`.

It is **not** a second transport, and it is important not to sell it as one. In duckdb v1.5.5 the
public appender has no direct write path at all: `Appender::FlushInternal`
(`duckdb/src/main/appender.cpp`) builds `INSERT INTO <db>.<schema>.<table>(cols) FROM <chunk>` and
hands it to `ClientContext::Append`, which plans it like any other statement. Against an mssql
catalog that reaches `MSSQLCatalog::PlanInsert` → `MSSQLPhysicalInsert` → batched `INSERT … VALUES`,
1000 rows at a time. The same wire shape the SQL batch already used.

What it avoids is **path resolution**. DuckLake's SQL batch resolves each file's path through the
*static* `GetPathForTable`, which has no cache and issues one `ducklake_table` and one
`ducklake_schema` query **per data file**; `TryAppendDataFiles` goes through the manager's *member*
`GetPath`, which caches in `table_paths` and asks once. Confirmed on the server rather than deduced —
one commit of 200 data files, read out of `sys.dm_exec_query_stats`:

```
SQL batch:   200 x SELECT [schema_id],[path],[path_is_relative] FROM ducklake_schema WHERE schema_id=@1
             200 x SELECT [schema_id],[path],[path_is_relative] FROM ducklake_table  WHERE table_id=@1
appender:    neither
```

Measured per commit, the lake prepared first so only the `INSERT` is inside the brackets, rounds
alternated, the first discarded, row counts verified before a timing is kept:

| data files | appender | round trips | SQL batch | round trips |
| ---: | ---: | ---: | ---: | ---: |
| 20 | 0.29 – 0.41s | **65** | 0.25 – 0.43s | 104 |
| 200 | 0.28 – 0.39s | **65** | 0.52 – 0.80s | 464 |
| 1000 | 1.36 – 1.69s | **66** | 2.50 – 2.66s | 2065 |

Round trips per commit stop growing with the commit. At twenty data files the two are level; at a
thousand the appender is 1.7x faster and asks the server for 66 things instead of 2065.

It composes with phase 2 rather than competing: `try_append_data_files` is wired only into
`RunCommitLoop` (`ducklake_transaction.cpp`), and `FlushChangesServerSide` never consults it. So the
appender improves exactly the path the server-side apply declines or falls below its threshold.

Two edges were checked because the size benchmark exercises neither — both behave identically to the
SQL batch, and both are now in the integration test:

- a table **created and filled in one transaction**, where the appender writes `ducklake_data_file`
  rows for a table the catalog does not know yet;
- a **rollback**, where the appended rows have to go with it — they do, since the appender writes
  through the metadata connection.

Two things it could plausibly have broken, checked rather than assumed:

- **The phase-2 server-side apply**, whose staging bulk-loads through BCP and is therefore sensitive
  to the column types D4 changes: the whole suite passes with `MSSQL_DUCKLAKE_SERVER_COMMIT=1`
  (`make test-integration-fast-path`), all 163 assertions.
- **Concurrent writers**, since the appender changes what a commit sends. Four writers, ten
  file-backed commits each, five runs per arm: three of five runs lose one writer to
  `INTERNAL Error: Failed to commit DuckLake transaction` — **with the appender and without it
  alike**. That is the pre-existing concurrent-commit failure (specs/005 D14, the conflict check
  that depends on `UNION ALL` row order), not a regression, and the arms are indistinguishable.

It is worth being explicit about one property this gives up. CLAUDE.md's "Execute-passthrough kills
the PK problem" rested on duckdb's DML path never being involved in a catalog write; the appender is
now the exception, because it INSERTs rather than passing raw T-SQL through `Execute`. That is safe
for the specific reason that mssql requires a key for UPDATE and DELETE but not for INSERT, and the
four tables the appender writes carry primary keys regardless — but the blanket statement was no
longer true and has been corrected.

### D2 — `INSERT … SELECT` does not use BCP, and never did

Checked at plan formation, because the claim kept being repeated: `MSSQLCatalog::PlanInsert` has no
BCP branch. It always ends in `planner.Make<MSSQLPhysicalInsert>(...)`, whose `Sink` calls
`executor->Execute(chunk)` — `INSERT … VALUES` batched 1000 rows at a time and capped at 8MB of SQL.
`mssql_ctas_use_bcp` is consulted by `PlanCreateTableAs` only. Corroborated on the server: inserting
5000 rows leaves **5 INSERT statements** in `sys.dm_exec_query_stats`, where BCP would leave none.

So the paths are:

| path | protocol |
| --- | --- |
| DuckLake's commit batch through `Execute` | raw T-SQL: multi-row `INSERT … VALUES` |
| DuckDB DML against the attached catalog (the appender included) | `INSERT … VALUES`, 1000 rows per statement |
| `CREATE TABLE … AS SELECT` | **BCP** by default (`mssql_ctas_use_bcp`) |
| `COPY … TO 'mssql://…' (FORMAT 'bcp')` | **BCP** — what specs/005's staging uses |

Bulk copy therefore remains reachable only through CTAS and explicit `COPY`, which is where phase
2's staging already gets it. Routing the commit's own rows through BCP is still open, but it is no
longer the obvious win it looked like: after D1 a thousand-file commit sends 66 round trips, and the
rows are no longer the dominant term.

### D3 — `MERGE INTO` works on lake tables

DuckLake plans `MERGE` itself (`ducklake_merge_into.cpp`), so the question was whether the commit it
produces survives this manager: a `MERGE` that updates rows writes a delete file **and** a data file,
a shape the phase-2 apply declines. It does survive. Every clause was checked against a plain
in-memory DuckDB table carrying the same rows through the same statements — no hand-computed
expected sums — and the lake matched row for row:

| shape | result |
| --- | --- |
| `WHEN MATCHED THEN UPDATE` + `WHEN NOT MATCHED THEN INSERT`, file-backed | identical |
| `WHEN MATCHED THEN DELETE` | identical |
| `WHEN NOT MATCHED BY SOURCE THEN DELETE` | identical |
| upsert on an inlined table | identical |
| all of the above with the server-side apply armed | identical — it declines the shape, as it must |

It costs more than postgres. Rounds alternated, first discarded, seconds:

| shape | mssql | postgres |
| --- | ---: | ---: |
| upsert (1000 rows, 1000 source rows) | 0.169 – 0.529 | 0.035 – 0.038 |
| delete matched | 0.037 – 0.042 | 0.011 – 0.012 |
| delete not matched by source | 0.024 – 0.029 | 0.007 – 0.008 |
| upsert on an inlined table | 0.048 – 0.091 | 0.010 – 0.015 |

That is the same per-statement gap specs/005 D13 measured for reads, not something specific to
`MERGE`: these commits take the client loop, and each carries a handful of statements.

### D4 — no `NVARCHAR` in the catalog

mssql maps DuckDB's `VARCHAR` to `NVARCHAR`, so DuckLake's catalog arrived as **51 `nvarchar(max)`
columns under `SQL_Latin1_General_CP1_CI_AS`**. Both halves of that are wrong here. UTF-16 spends two
bytes per character on content that is paths, type names and identifiers; and a case-insensitive
linguistic collation is a comparison DuckDB does not have, so the server was the one behaving
differently. The six statistics columns were already converted for exactly this reason (specs/004
D3) — the rest simply had not been.

Every string column is now `VARCHAR` under `Latin1_General_100_BIN2_UTF8`. The conversion is
**generated from `sys.columns`** rather than listed in our source: the list is DuckLake's, it moves
with every submodule bump, and a column added upstream would otherwise silently keep the wrong type.
`ducklake%` in the metadata schema is the extension's namespace rather than a guess — DuckLake
creates and drops tables under that prefix there itself, so a table of someone else's answering to it
would already be colliding with DuckLake.

Once converted the sweep matches nothing, so it costs one statement on later attaches — but
`ALTER COLUMN` **rewrites the table**, so the first attach after an upgrade walks the whole catalog.
Measured on a 198 MB catalog holding 500,001 file-column-stats rows: **7.65s for that attach against
2.65s for the next one**, so about five seconds of one-time conversion, after which the lake reads
normally and no `nvarchar` column is left. It is a one-time cost on the first attach, not a per-attach
one, but it is not free and an operator upgrading a large catalog should expect it.

Non-ASCII is what makes this safe or not, and it is now asserted: a table name, a column name and
values in Cyrillic, an accented Latin string, CJK and a 4-byte emoji all round trip.

One consequence worth knowing: a query joining a catalog string column to a system view now needs
`COLLATE DATABASE_DEFAULT`, because `sys.tables.name` carries the database's collation and SQL Server
refuses to compare two explicit collations without being told which wins. Nothing in the manager does
such a join; the integration test did, and says so.

**`partition_value` is bounded to `VARCHAR(200)`**, which is what makes partition pruning indexable
at all — unbounded it is a LOB, and a LOB can be neither an index key nor an `INCLUDE`. A partition
key is a date, a tenant or a bucket; 200 bytes is far more than one ever is, and a longer one is
refused by the server rather than silently truncated.

`ix_ducklake_file_partition_value_lookup` on `(table_id, partition_key_index, partition_value)`
follows. DuckLake turns a filter on a partition key into

```sql
SELECT data_file_id FROM ducklake_file_partition_value
WHERE table_id = ? AND partition_key_index = ? AND partition_value IN (…)
```

and the primary key is `(data_file_id, partition_key_index)`, whose leading column is not in that
predicate — the same shape that made the file-column-stats index worth 15x. Measured over 1,000,000
partition-value rows spread across 1000 tables (selectivity matters: with everything under one
`table_id` an index like this measures as worthless), rounds alternated:

```
no index   0.037 – 0.038s
indexed    0.000 – 0.001s
```

37x. With only `(table_id, partition_key_index)` indexable — the best available while the column was
a LOB — the same query was 0.008s → 0.003s, so most of the win comes from bounding the column.

**The shape probe is now versioned.** `CatalogShapeIsCurrent()` used to check that one constraint
exists, which answers *"some build of this extension shaped this catalog"*. A catalog shaped by an
older build therefore read as current and never received new column types or indexes — every shape
change to date would have reached new catalogs only. It now compares a version stamped in an extended
property on the schema, written last, after the DDL that earns it. The upgrade path is a test: the
stamp is removed and a column put back the way DuckLake declares it, and the next attach restores
both.

### D5 — a table created and written in one commit was invisible to its own INSERT

Found by testing `MERGE`, and present since the manager was written — not a consequence of anything
above.

DuckLake's commit does this (`ducklake_transaction_state.cpp`):

```cpp
batch_queries += CommitChanges(...);            // creates the inlined table, marks the cache pending
auto res = context.execute_commit_batch(...);   // INSERTs into it
context.flush_cache_if_pending();               // the cache clear — after the batch that needed it
```

The mssql extension caches catalog metadata, and our inlined data table is created behind its back
through `mssql_exec`. So a table created **and** written in the same commit — which is every
`CREATE TABLE … AS SELECT` under the inlining limit — could fail with

```
Failed to flush changes into DuckLake: Table with name ducklake_inlined_data_2_2 does not exist!
```

It only showed once the extension's cache was warm for that schema, which is why simple sequences
pass and a sequence with real work in front of it does not. The base's *deletion* table path never
had the problem, because it invalidates there itself, immediately after creating the table.

The fix is the same: `GetInlinedTableQueries` now invalidates that one table's cache entry at the
moment it creates it, instead of only recording it for DuckLake's post-batch clear. Safe where a
schema-wide clear at that point is not — the table is created *outside* the transaction
(`RunServerSideOutsideTransaction`), so it is committed and holds no lock for the extension's
metadata read, on its own connection, to wait on. The name is still recorded, so DuckLake's own clear
stays the cheap targeted one; the repeat costs a single round trip.

## Enforcement & security

Unchanged from specs/005: rows travel as **data**, never as statement text the server has to parse as
SQL. The appender strengthens this rather than weakening it — values are typed through mssql's codecs
instead of being formatted into a literal by DuckLake.

## Testing

`test/sql/integration/attach_mssql.test`, 157 assertions (was 100): the appender's rows and file
count, a table created and filled in one transaction, a rollback leaving no orphaned file rows, all
four `MERGE` shapes, the non-ASCII round trip, zero `nvarchar` and zero non-BIN2 string columns,
`partition_value` as `varchar(200)`, the partition index, the shape stamp, and the upgrade path.

Everything measured here followed the discipline specs/005 arrived at the hard way, and it paid twice
in this session alone — a benchmark that silently attached nothing was caught by its row count, and a
`MERGE` expectation computed by hand was caught by computing it in DuckDB instead:

- rounds **alternated** between variants, never one variant then the other;
- the first round **discarded**;
- the result **verified** before a timing is kept;
- a **realistic distribution** — the partition index measures as worthless with every row under one
  `table_id`;
- and on a catalog big enough to answer the question, which for the write path means
  `make bench-scale`. That benchmark now builds partitioned tables too: nothing else in it produced a
  commit carrying many data files, and partitioning is what multiplies a catalog rather than adding
  to it — one commit into a wide, 25-way partitioned table writes as much catalog as twenty-five
  ordinary ones.

The partitioned phases, on 200 tables over 10 schemas with 41 columns each, ten of them partitioned
25 ways and taking five commits each — 1250 data files and ~52,500 file-column-stats rows from those
commits alone:

| phase | mssql | postgres | ratio |
| --- | ---: | ---: | ---: |
| `partitioned_create` | 2.51s | 1.15s | 2.18x |
| `partitioned_commits` | 17.06s | 2.90s | 5.89x |
| `partitioned_read_latest` | 0.666s | 0.065s | 10.25x |
| `partitioned_read_pruned` | 0.082s | 0.009s | 9.11x |
| `partitioned_reattach` | 2.88s | 0.083s | 34.64x |
| `partitioned_merge_adjacent` | 16.00s | 2.53s | 6.33x |

Pruning does its job — the pruned read is 8x cheaper than the full read of the same table — and the
ratios against postgres are the per-statement gap of specs/005 D13 again, not anything specific to
partitioning.

## Alternatives considered

- **Routing the commit's own rows through BCP.** Still open, but demoted by D1: the round trips it
  would save are already gone, and BCP inside the commit means staging tables, which is phase 2's
  design and phase 2's cost.
- **Making the commit batch smaller** by trimming what DuckLake writes: not ours — the batch is
  DuckLake's own SQL, and the vendoring rule keeps us out of it.
- **`INCLUDE`ing `partition_value` instead of bounding it**: impossible while it is a LOB, which is
  the same constraint that keeps `min_value`/`max_value` out of the stats index.

## Follow-ups

- specs/005's open items that belong to writing: the apply's scope beyond data files, and the
  server-side retry.
- Upstream findings, none of them ours to fix, to raise with DuckLake once asked: the uncached
  per-file path resolution in `write_data_files_sql` (D1 — the appender sidesteps it, the SQL batch
  still pays it), the empty inlined tables left by a flush (specs/005 D10), the catalog reloaded once
  per schema version (D12), and the conflict check that depends on `UNION ALL` row order (D14).
