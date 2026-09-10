# Spec 009: the first inlined write, and what an attach costs

- **Status**: draft
- **Date**: 2026-09-10
- **Author**: VGSML

## Summary

The first inlined insert into a table costs seven times the second one, and on a thousand tables
that is the worst write-side ratio in the benchmark: `first_commits` at 9.55x postgres. The
difference is the metadata of a table that did not exist a moment ago. This spec measures where
that load actually happens before choosing between two fixes, and takes the attach cost — 534 ms
of the extension's own catalog preload, 6 – 13x on `reattach` — along with it, since it is the same
mechanism.

## Problem

From the 1000-table catalog (design 002 §1.1), one statement each, TDS batches counted with
`MSSQL_DEBUG=1` and timings taken without it:

| statement | batches | time | of which the commit batch |
| --- | ---: | ---: | ---: |
| first inlined insert into a table | 33 | ~150 ms | **79 ms** |
| second inlined insert into the same table | 19 | 22 ms | 20 ms |
| file-backed insert | 19 | 22 ms | 22 ms |

The first insert creates `ducklake_inlined_data_<t>_<v>` outside the transaction
(`RunServerSideOutsideTransaction`), invalidates the extension's cache entry for it
(`InvalidateTableCache`, specs/006 D5), and then the commit batch INSERTs into it through DuckDB's
DML path — which needs the table's metadata, which the extension has to load first. The extra
~60 ms in the batch is that load. Around it, DuckLake reloads its own catalog (~90 ms, six queries,
specs/005 D12 — not ours).

`make bench-scale` sees it as 1000 first inserts: `first_commits` 55.7 s against 5.8 s. Every
table a lake will ever inline into pays it once.

The attach is the same mechanism at a different scale: `ATTACH OR REPLACE 'mssql:…'` takes 534 ms
on this catalog, the extension preloading metadata for 2000+ objects, before DuckLake has asked it
anything. `reattach` 6.2x, `deep_reattach` 13.1x, `partitioned_reattach` 12.2x.

## Design

**Measure first; the fix depends on the answer.** The open question is what
`mssql_invalidate_cache(catalog, schema, table)` costs on the next touch: the load of that one
table, or a reload of the whole schema the way a schema-wide clear does (`ClearCache` measured that
at 39 round trips on a small catalog). The measurement is the batches — and their SQL — between
`InvalidateTableCache` and the first `INSERT` into the inlined table, on the 1000-table catalog,
with `MSSQL_DEBUG=2`.

Then, by outcome:

- **If the invalidation costs a schema-wide reload**: prime narrowly. After `CREATE`, ask the
  extension for exactly that table's metadata and nothing else, so the DML finds it loaded. What
  call does that is the extension's to answer (`mssql_preload_catalog` takes a schema; a per-table
  form may need to be asked for in `hugr-lab/mssql-extension`); this spec records the number and
  the ask.
- **If it is the single table's load and that load is inherently ~60 ms**: the cost is in the
  extension's metadata query per table, and the lever is there, not here. This spec records the
  per-table cost against the extension's introspection query so the ask is precise.
- **Either way**, the inlined DDL the manager issues is already minimal (one `CREATE TABLE` with a
  primary key); nothing to trim on our side.

**Attach.** The ducklake attach passes no options through to the inner `ATTACH 'mssql:…'`, so the
extension's `lazy_validation` and `catalog` options are unreachable from `ducklake:mssql:`. Two
things to measure: what the preload costs with and without those options on a plain `ATTACH` of the
same catalog, and whether the manager needs the preload at all — it addresses every catalog table
by name, and the extension resolves a named table on demand. If the second holds, the ask is a way
to pass attach options through DuckLake's `ATTACH` (a DuckLake question) or a default the extension
applies when the attaching client is this manager.

## Enforcement & security

No new SQL surface. A narrow prime, if that is the fix, names a table the manager just created
itself.

## Testing

- The batch count between `CREATE` of the inlined table and the first `INSERT` into it, before and
  after, on the 1000-table catalog — the number this spec exists to move.
- `make bench-scale`: `first_commits` and the three `reattach` phases, rounds alternated with the
  build before the change.
- The integration suite unchanged: the inlined-table path is covered end to end (creation in the
  same commit as the first write, specs/006 D5; MERGE on inlined tables, D3).

## Alternatives considered

- **Skip the inlined table on first write** — inline only from the second insert on. Changes
  DuckLake semantics (the inlining limit) for a cost we have not yet located; rejected until the
  measurement says the load is unavoidable.
- **Create all inlined tables at `CREATE TABLE` time** rather than on first insert. Moves the cost
  to `create_tables`, which is already 1.67x, and creates tables most lakes never inline into.

## Follow-ups

- Whatever the measurement asks of the extension goes to `hugr-lab/mssql-extension` with the
  numbers attached.
- The DuckLake-side catalog reload per schema change (specs/005 D12) sits in the same 150 ms and
  is not addressed here.
