# Spec 012: forced parameterization on the catalog's database

- **Status**: implemented
- **Date**: 2026-09-10
- **Author**: VGSML

## Summary

Every query DuckLake and the mssql extension send the catalog carries its literals in the text —
a table name in the extension's metadata query, `WHERE table_id = 1053` in DuckLake's own — and
SQL Server caches ad-hoc plans by text. Each distinct value is therefore a plan of its own, and its
first execution compiles it: 28 – 37 ms for the metadata query, measured on the optimizer's counter
(specs/009). The proper fix for the extension's two templates is theirs (hugr-lab/mssql-extension#334)
and waits for the 2.0 line; DuckLake's own literals have no such fix at all. The database has one
for both: `ALTER DATABASE … SET PARAMETERIZATION FORCED`, after which the server parameterizes
the literals itself and one plan serves every value. Measured on the 1000-table benchmark before any
code was written: **937 s → 686 s**, the first write into each table three times faster, the first
read after an attach five times.

This spec makes shaping a catalog apply that option to the catalog's database — best-effort, once
per shape version, with an opt-out — and bumps the shape version so that catalogs shaped by earlier
builds pick it up on their next attach.

## Problem

Where the compiles land on the 1000-table catalog (specs/009): an attach and its first read pay 24
of them (≈ 0.9 s of 1.9 s); every first write into a table pays one (the entire premium of
`first_commits` over `second_commits`, 57 vs 37 ms per commit); and every commit, every read, every
maintenance function pays one per literal it carries that the server has not seen — which is why
`second_commits`, `flush_inlined` and `deep_history` moved in the measurement above when no
first-touch count explains them. On this server the plan cache holds ~53 plans, so nothing survives
to the next session; on a larger one `optimize for ad hoc workloads` — common in production — makes
the first execution a stub and the second a compile, so a touch-once pattern compiles regardless.

## Design

**D1 — applied where the catalog is shaped.** `EnsureCatalogShape` already shapes the catalog's
database for SQL Server — keys, the UTF-8 BIN2 `VARCHAR`s, indexes — once per catalog per shape
version, at creation and at the first attach with a build whose `SHAPE_VERSION` is newer. The
option is its last step, `ApplyForcedParameterization`, and `SHAPE_VERSION` goes to 3, so a catalog
shaped by an earlier build is re-shaped once (the DDL is idempotent) and gets the option too.

Not on every attach. A DBA who sets the database back keeps it back; the manager applies the option
when it shapes, and the shape is stamped. Re-applying per attach would have the extension fight the
DBA with no channel to say so.

**D2 — outside the transaction, and online.** `ALTER DATABASE` is refused inside a transaction, so
the statement runs through `RunServerSideOutsideTransaction` on a connection of its own, in
autocommit, like the inlined-table DDL. The option is online: measured against a session holding
uncommitted DDL and a row lock in the same database, the `ALTER` completed in a second — the pinned
transaction the attach holds does not block it. `ALTER DATABASE CURRENT` names the connection's
database, which is the catalog's, so the manager never has to spell a database name.

**D3 — best-effort.** The statement needs `ALTER` on the database, which a login allowed to create
the catalog's tables need not have, and the option does not exist everywhere: the `ALTER DATABASE
SET` reference lists `PARAMETERIZATION` for SQL Server, Azure SQL Database, Azure SQL Managed
Instance and SQL database in Fabric, not for the Synapse dedicated pool, and Fabric Warehouse has no
`ALTER DATABASE` in its T-SQL surface at all. A failure is caught, the catalog is complete and
stamped without the option, and a warning goes to DuckDB's log (`duckdb_logs()` with
`enable_logging`) naming the statement to run by hand. Verified with a login holding `db_ddladmin`
and the data roles but not `ALTER`: the catalog is created and written, `is_parameterization_forced`
stays 0, the warning is logged.

**D4 — the opt-out.** A database-wide option deserves one: `mssql_ducklake_forced_parameterization`
(BOOLEAN, default true, global scope), registered by the extension and read at shaping time. False
means the shaping leaves the database's setting as it is — it never sets `SIMPLE` — and stamps the
shape regardless, so opting out is a decision for that catalog, not a deferral.

**D5 — what it does to DuckLake's own queries.** More than the extension's two templates: the
bench moved on phases with no first touches in them, because DuckLake's own `WHERE table_id = ?`,
`snapshot_id = ?`, and inlined-table names are ad-hoc texts as well. The risk forced
parameterization carries in general — a plan compiled for one value reused for a value with a very
different distribution — is small here: the catalog's predicates are key lookups and visibility
ranges over indexed columns, and no phase of the benchmark regressed (`evolution_read_latest`
24.0 → 25.1 is inside the run-to-run spread).

**D6 — the platform matrix**, per the `ALTER DATABASE SET` reference (`view=sql-server-ver17`,
whose sections carry per-platform syntax):

| platform | `PARAMETERIZATION` | what happens here |
| --- | --- | --- |
| SQL Server 2019+ | yes | applied |
| Azure SQL Database, Managed Instance | yes | applied (`ALTER DATABASE CURRENT`) |
| SQL database in Fabric | yes | applied |
| Fabric Warehouse | no `ALTER DATABASE` | fails, logged, catalog complete |
| Azure Synapse dedicated pool | not in its option list | fails, logged, catalog complete |

## Enforcement & security

- The one statement the manager runs outside the catalog's own schema. It changes a database-wide
  optimizer setting for every workload in that database; the opt-out and the README are the
  disclosure, and the choice of a dedicated database for the catalog — the common case — makes it
  moot.
- Needs `ALTER` on the database; without it nothing is attempted twice and nothing fails.
- No new SQL surface, no user input in the statement.

## Testing

- `test/sql/integration/attach_mssql.test`: the database is set back to `SIMPLE` before the run, so
  the assertion that the first attach set `is_parameterization_forced` proves this run did it; the
  shape stamp reads 3; the older-build re-shaping runs with the opt-out and leaves the database at
  `SIMPLE`; opted back in and re-stamped, the next shaping applies it.
- The best-effort path by hand (D3), with a `db_ddladmin` login.
- `make bench-scale --tables 1000 …` on this build: the catalog it creates is shaped with the option
  by default, so the bench measures the shipped behaviour (table below).

## Alternatives considered

- **Re-apply on every attach** when the probe says `SIMPLE`. One cheap read per attach, but it
  fights a DBA who chose otherwise; rejected for D1.
- **A `TEMPLATE` plan guide** for the extension's two query texts (`sp_get_query_template` +
  `OPTION (PARAMETERIZATION FORCED)`): narrower than the database option, but it did not match the
  extension's text in the one attempt, and it would not cover DuckLake's own literals, which D5 shows
  are most of the gain.
- **Wait for hugr-lab/mssql-extension#334** alone: it fixes the two templates on the 2.0 line and
  nothing of DuckLake's; still worth having for databases where the option is refused.
- **Parameterize DuckLake's queries** ourselves: a rewrite of every literal-carrying query the
  embedded ducklake generates — the transpiler specs/004 rejected, for a gain the database option
  gives for free.

## Follow-ups

- hugr-lab/mssql-extension#334 stays open: on a database where `ALTER` is refused, it is the only
  fix for the metadata query.
- specs/010: CLAUDE.md's description of the shaping gains this step.
