# Spec 009: the first inlined write is a plan compile per table

- **Status**: implemented (reconnaissance; the fix is the extension's — hugr-lab/mssql-extension#334)
- **Date**: 2026-09-10
- **Author**: VGSML

## Summary

The draft of this spec expected the targeted cache invalidation to re-list the whole schema, and
planned a narrow prime after `CREATE`. Measured, that is not what happens: the extension loads
exactly one table on a miss, and `mssql_invalidate_cache(catalog, schema, table)` keeps the
schema's table list. The premium of the first inlined insert is one thing — the **first execution
of the extension's single-table metadata query for that table name**. The query text carries the
name as a literal, so every table is its own ad-hoc plan, and the first execution compiles a join
over five catalog views: **28 – 37 ms**; the same text again: **1 ms**. The primary-key discovery
text is the same shape at 45 – 51 ms, though the write path does not run it.

Nothing in this repository moves that cost; the fix is the extension's (parameterize the two
templates, #334 — which needs the RETURNSTATUS fix of #332 first, so it lands on the 2.0 line).
The database setting `PARAMETERIZATION FORCED` gives the same effect without any code and is
measured here; whether to apply or recommend it is its own spec. What this spec changes is the
record — the anatomy in specs/005 – 006 and design 002 §1.1 — and the instrument: `metadata_log.py`
parsed exactly one entry per run before this.

## The measurement

One session on the 1000-table catalog `make bench-scale` leaves behind, `MSSQL_DEBUG=2`, one
`CREATE TABLE` then two inlined inserts into it; timings from the `DuckLakeMetadata` log of a
second session without the debug flag.

| statement | what the stream shows | commit (log) |
| --- | --- | ---: |
| `CREATE TABLE` | the inlined table is created here, through `mssql_exec`, before the commit (DuckLake creates it with the table, specs/006 D5); six `INSERT`s through DuckDB's DML path, three single-table loads at 1 ms — texts the server had compiled minutes earlier; then DuckLake's own catalog reload, 44K rows of `ducklake_column` | 220 ms |
| first inlined `INSERT` | `GetEntry('dbo.ducklake_inlined_data_1053_1206')` — a miss, **one** single-table load, **40 ms**, the only new thing; the `INSERT` into it; the commit | 48 ms |
| second inlined `INSERT` | cache hit; the commit | 25 ms |

No `TABLE_DISCOVERY` after the attach. The targeted `mssql_invalidate_cache` the manager issues
when it creates the inlined table (specs/006 D5) leaves no line of its own — the extension does
not log it — but what it did is in what follows: one single-table load on the next touch, no
re-listing. The draft's question is answered: it is the single table's load, and the load is the
compile.

The compile, isolated, four ways:

| how | first execution | second |
| --- | ---: | ---: |
| the template text through `mssql_scan`, `sys.dm_exec_query_optimizer_info` around it | 34 ms, `optimizations` +1 | 1 ms, +0 |
| `SET STATISTICS TIME ON` (sqlcmd) | parse and compile 28 ms, execute 1 ms | |
| the extension's own load, three untouched tables in a fresh session (`RunMetadataQuery: completed in`) | 37 / 32 / 32 ms | after a targeted invalidate: 1 ms |
| the same query as `EXEC sp_executesql … @n` (sqlcmd, plain batch) | 29 ms once | **0 – 1 ms for every other table** |
| the extension unchanged, `ALTER DATABASE … SET PARAMETERIZATION FORCED` | 38 ms once | **1 / 1 ms** |

Why it is paid every session: this server's whole plan cache holds ~53 plans (611 MB of server
memory in a 2 GB VM), so per-table plans do not outlive the session. A larger server keeps them
longer, but with `optimize for ad hoc workloads` on — common in production — the first execution
stores a stub and the second compiles again, so a touch-each-table-once pattern compiles for every
table regardless. `sp_executesql` through `mssql_scan` fails today with
`TDS parse error: Unknown token type: 0x0` — the RETURNSTATUS desync of hugr-lab/mssql-extension#323,
fixed by #332; that is why the extension-side fix waits for the 2.0 line.

Where it lands, on the same catalog:

| | first touches | cost |
| --- | ---: | ---: |
| `ATTACH` + first read | 24 single-table loads | ≈ 0.9 s of 1.9 s (`filtered_read`) |
| `first_commits`, 1000 tables | 1000 | the whole premium over `second_commits`: 57 vs 37 ms per commit (postgres 6 vs 10) |
| one read of a table whose inlined data spans 154 schema versions | 154 | see below |

`mssql_preload_catalog('__ducklake_metadata_lake', 'dbo')` after the attach is not a way around
it: 0.95 s itself over ~1200 tables, and it saves ~0.5 s on the reads that follow.

### The database setting, whole bench

`ALTER DATABASE lake_meta SET PARAMETERIZATION FORCED`, then `make bench-scale … --backends mssql`
on a fresh catalog with the profile above, the setting reverted afterwards. One run, single-shot
phases, so the spread between two plain runs (874 vs 937 total) is the noise floor.

| phase | main | with `FORCED` | postgres |
| --- | ---: | ---: | ---: |
| `first_commits` | 56.9 | **19.2** | 5.9 |
| `second_commits` | 36.7 | 24.7 | 9.9 |
| `filtered_read` | 1.91 | **0.40** | 0.19 |
| `list_snapshots` / `table_info` | 0.18 / 0.27 | 0.02 / 0.03 | 0.01 / 0.01 |
| `partitioned_reattach` / `deep_reattach` | 2.55 / 2.68 | 1.09 / 0.97 | 0.24 / 0.25 |
| `flush_inlined` | 366 | 245 | 153 |
| `deep_history` | 114 | 77 | 32 |
| `merge_adjacent` | 44.8 | 33.0 | 22.5 |
| `evolution_read_latest` | 24.0 | 25.1 | 8.0 |
| **total** | **937** | **686** | **385** |

More moves than the two templates account for. Every literal-carrying query DuckLake itself issues
— `WHERE table_id = 1053`, `WHERE snapshot_id = 6411`, the inlined-table names — is an ad-hoc text
of its own too, and the setting parameterizes those as well; that is why `second_commits`,
`flush_inlined` and `deep_history` move when no first-touch count explains them.
`evolution_read_latest` does not move: its cost is executing 155 catalog loads, not compiling
them. This is the measurement the follow-up spec on the setting starts from.

## What else the stream showed

Recorded here because the same measurement found them; none is fixed by this spec.

- **The commit runs statement by statement through DuckDB's DML path.** That is specs/004's
  decision (`Execute` is not overridden — "no transpiler"), and it is what the stream shows: ~19
  round trips per commit (BEGIN, a SET batch, COMMIT, one batch per `INSERT`/`UPDATE`), and the
  column-stats `UPDATE … FROM new_values` is a join, so nothing pushes down and every commit scans
  all of `ducklake_table_column_stats` — 41.8K rows on this catalog, ~10 ms warm, growing with the
  catalog. The postgres manager hands the whole batch to `postgres_execute` in one round trip;
  that is the 3.7 – 5x on `second_commits`, `partitioned_commits`, `deep_history`. CLAUDE.md's
  "Execute-passthrough" bullet describes what 004 rejected; specs/010 corrects it. DuckLake has the
  seam for a server-side commit — `FlushChangesServerSide`, which the quack manager uses — and
  design 001 §7 has the plan; that is a spec of its own.
- **`evolution_read_latest` is 155 full catalog loads.** The table's inlined data spans 154 schema
  versions, and DuckLake reads each inlined table with the catalog as of its version: six
  catalog-path queries per load, ~90 ms single-threaded here and 1.3 s under eight threads, 240 s of
  metadata time inside a 30 s read. The postgres backend does the same 155 loads in 8 s. Upstream
  mechanism; our lever is the price of one load.
- **The attach** is 0.7 – 0.8 s: the extension's schema discovery, then the same single-table
  loads for every catalog table DuckLake touches — the compile again, 24 times.

## Design

What changes in this repository:

- `scripts/bench/metadata_log.py` had two bugs that between them made it return one entry per run:
  the record separator was the SQL literal `'\x1e'` — four characters to DuckDB — and a chunk's
  trailing quote-newline-quote was trimmed in the wrong order, so only the last row matched. Fixed
  (`chr(30)`; strip the three together). Validated on the evolution read: 1556 queries, 16 shapes,
  the 155 catalog loads visible.
- The anatomy in design 002 §1.1 and §4, and the draft of this spec, replaced by the measurement.
- No manager change. The inlined DDL is one `CREATE TABLE` with a primary key, created with the
  table; a prime after `CREATE` would pay the same compile the `INSERT` pays.

## Enforcement & security

No new SQL surface. The measurement scripts run as the catalog's owner; `PARAMETERIZATION FORCED`
was set and reverted on the integration server only.

## Testing

- `make metadata-log WORKLOAD=…` returns the full log — the instrument's own regression is the
  count it prints.
- The integration suite unchanged; no behaviour changes.

## Alternatives considered

- **A narrow prime after `CREATE`** (the draft's plan): the prime is the same query the `INSERT`
  triggers; it moves the 40 ms, it does not remove them.
- **Inline only from the second insert on**: still changes DuckLake's inlining semantics for 40 ms
  the extension will remove; rejected.
- **A `TEMPLATE` plan guide installed by `InitializeDuckLake`** (`sp_get_query_template` +
  `OPTION (PARAMETERIZATION FORCED)`): did not match the extension's text in the one attempt;
  the database-level setting did. Whether that setting is ours to apply, recommend, or probe at
  attach is the follow-up spec.

## Follow-ups

- hugr-lab/mssql-extension#334 — parameterize `SINGLE_TABLE_METADATA_SQL_TEMPLATE` and
  `PK_DISCOVERY_SQL_TEMPLATE`; after #332.
- `PARAMETERIZATION FORCED` on the catalog database: probe, recommend or apply — the decision, from
  the bench above (−27% total, `first_commits` 3x).
- Server-side commit (`FlushChangesServerSide`) — the write-side gap the DML path leaves.
- specs/010: the CLAUDE.md correction.
