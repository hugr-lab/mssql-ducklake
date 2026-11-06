# Spec 022: importing a full SQL Server backup, one DuckLake snapshot per file

- **Status**: draft
- **Date**: 2026-11-05
- **Author**: vgsml, with Claude

## Summary

`PRAGMA ducklake_import_mssql_backup('lake', 'sales_full.bak')` turns a full backup file into one DuckLake
snapshot, with no SQL Server instance anywhere. The work is done in three phases:

- **Plan.** Read the backup's header and catalog through mssql's file functions (085), and read the lake's
  state at its latest snapshot.
- **Export.** Parallel workers write every data file. They run outside any lake transaction.
- **Register.** One transaction re-validates the plan first, then runs the DDL (and 023's deletes) and
  registers every file through DuckLake's own transaction (`AppendFiles`). It also creates the
  `__mssql_sys` mirror of the SQL Server base tables, the allocation images, and the provenance rows, and
  sets the commit message. In autocommit a DuckLake conflict at commit is retried, bounded, on the
  same export; inside an explicit transaction the COMMIT fails and a re-run resumes from it (D1).

The lake this spec creates is the base that 023 applies differentials and later fulls to, and its
snapshots are the backup history that 024 keeps and expires. This spec also fixes the decisions the lake
is born with: the service columns, the partitioning and sort order proposed from SQL Server once, at
creation (after that the layout is DuckLake's), inlining off, and DuckLake's standard compaction.

Owner decisions are cited as DQ-nn, from one list merged across both series:
[decisions.md](https://github.com/hugr-lab/mssql-extension/blob/main/specs/085-backup-reader-core/decisions.md).

**Owner decision (2026-11-05): follow DuckLake's rules, do not replicate SQL Server** (DQ-10). SQL
Server's partitioning only proposes the initial DuckLake spec; partition values are computed exactly
as DuckLake computes them, in the importing session; files are laid out by DuckLake partition
values, sort order and file size, never per SQL Server partition (those are work units only); a
SWITCH is a lake-side copy plus a `DELETE` by `_mssql_page` (023 D8).

## Problem

There is no way to get a SQL Server backup into a lake without a server: today it takes RESTORE, then
`mssql_scan` or COPY, table by table, with one snapshot per statement. The owner's operating model needs
more than a copy (RESEARCH §6.2):

- one snapshot per backup file, so that time travel *is* the backup history;
- a lake that can take the next differential with nothing but itself and that file. That needs row
  locators (`_mssql_rid`, `_mssql_page`), the SQL Server catalog, the allocation images, and the LOB
  fragment maps, all stored in the lake from the first import (E1 rules 5–8, E2 R5);
- an import that scales: 1 TB and thousands of tables, parallel across tables and within them, and a
  commit that does not hold a lake transaction open for hours.

DuckLake's own write paths do not fit:

- An `INSERT … SELECT FROM mssql_backup_scan` runs the whole import inside one lake transaction, and a
  rollback deletes every file written so far (`DL/storage/ducklake_transaction.cpp:839-853`).
- `COPY FROM DATABASE` puts the tables in one UNION ALL, whose saturating children run one after another
  (research-ducklake-import §3).

Paths: `DL/` = `ducklake/src/`, `DD/` = `duckdb/`, `MD/` = `src/` (mssql-ducklake at `8fe10a1`, ducklake
`dcf71031`, duckdb `d54cbbe3df1`).

## Design

### D1. The SQL surface: a PRAGMA that expands into one transaction

```sql
PRAGMA ducklake_import_mssql_backup('lake', 's3://bak/sales_full.bak',
    partitioning := MAP {'dbo.orders': 'epoch_month(order_date)', 'dbo.audit': 'none'},
    sorting      := MAP {'dbo.events': 'none'});
```

| option | default | meaning |
| --- | --- | --- |
| `files` | `[]` | the other members of a striped media set: all FamilyCount files, one MediaSetId (E2 R9; the reader refuses an incomplete set). Passed to the reader as one path list |
| `position` | 1 | the backup set's position in a file that holds several (`RESTORE … FILE = n`); the reader's parameter of the same name (085) |
| `schemas`, `tables` | all | regex filters, fixed at the first import and stored in `__mssql_backup.config`. A table added to them later needs every one of its pages, so 023 refuses a change unless the next full is applied in `rebase` mode |
| `partitioning`, `sorting` | proposed (D5) | per-table specs for the tables this call creates; `'none'` declares none. After creation the specs are DuckLake's, changed only with DuckLake's own ALTERs |
| `max_memory` | config `import.max_memory`, else DuckDB's `memory_limit` | the import's own memory bound, `<bytes or '2GB'>`; clamped to `memory_limit`, with a WARNING (D12) |
| `max_threads` | config `import.max_threads`, else DuckDB's `threads` | the import's own concurrency bound; clamped to `threads`, with a WARNING (D12) |
| `mode` | `'auto'` | `'fresh'` requires a lake without this database; 023 adds `'incremental'` and `'rebase'` |
| `if_applied` | `'skip'` | a file whose BackupSetGUID is already in `__mssql_backup.imports` is a no-op that returns its snapshot; `'error'` refuses it |
| `manifests` | NULL | register files written by external workers (D4) instead of exporting |
| `resume` | true | reuse the files of an unfinished run of the same file whose plan still validates (D9) |
| `run_path` | config `import.run_path`, else `<DATA_PATH's parent>/mssql_backup_runs/<lake id>/` (set explicitly when DATA_PATH is a bucket root, which has no parent) | where the run's scratch lives, outside DATA_PATH and apart from every other lake's (D9) |
| `dry_run` | false | expand to `ducklake_mssql_backup_plan(...)` only |

There is no decryption parameter. Keys come only from mssql's `mssql_backup_key` secret (087). There is
no file-size parameter: files follow the table's or the lake's DuckLake `target_file_size` (owner,
DQ-10). There is no `fuzzy` parameter either: an inconsistent file is always refused (D11; the owner's
no-dirt decision, R§4); the counterfactual that imports one anyway is a test-only switch.

**Why a PRAGMA.** One snapshot per file means the DDL and the file registration must be in one
transaction. That rules out the other two candidates:

- **A table function** cannot run statements (`CALL` binds as `SELECT * FROM f()`). It could do the DDL
  through the Catalog API, but that means binding `CREATE TABLE … PARTITIONED BY … SORTED BY … WITH (…)`
  by hand, while the DuckDB SQL for it is the public, stable contract
  (`DD/src/include/duckdb/parser/parsed_data/create_table_info.hpp:40-44`).
- **A plain SQL script** would leave the user writing the DDL.

A PRAGMA's callback returns SQL, and DuckDB runs the expanded statements "as one query in one transaction"
(`DD/src/planner/statement_preprocessor.cpp:56-66`). `copy_database` is the precedent. The expansion is
inspectable (`dry_run` shows it), and it works in autocommit and inside an explicit transaction. In the
explicit case the snapshot comes at the user's COMMIT, along with anything else the user did.

The cost is that the callback plans at one snapshot and the statements run at a later one. The
transaction re-validates first (D2.3).

**The expansion** (phase B; phase A is in D2.4), as it runs inside an explicit transaction; in autocommit
the statements after the export run through `ducklake_mssql_backup_commit` (below):

```sql
-- the callback (D2.1) plans at the lake's latest snapshot S0 through a side connection and writes
-- <run_path>/<BackupSetGUID>/<run>/plan.json (D9); then:
CREATE TEMP TABLE __mssql_backup_manifest_<run> AS
    SELECT * FROM ducklake_mssql_backup_export('lake', 's3://bak/sales_full.bak', plan := '<run dir>/plan.json');
SELECT * FROM ducklake_mssql_backup_validate('lake', 's3://bak/sales_full.bak', plan := '<run dir>/plan.json');
CREATE SCHEMA lake.__mssql_sys;  CREATE SCHEMA lake.__mssql_backup;  CREATE SCHEMA lake.dbo;
CREATE TABLE lake.dbo.orders (id BIGINT, order_date TIMESTAMP, …, _mssql_rid BLOB, _mssql_page VARCHAR)
    PARTITIONED BY (epoch_month(order_date)) SORTED BY (id)
    WITH (data_inlining_row_limit = 0, skip_stats_columns = ['_mssql_rid']);
-- … every table, every mirror, alloc_pages, pending_pages, stale_pages, the registry tables
SELECT * FROM ducklake_mssql_backup_apply('lake', 's3://bak/sales_full.bak',
    manifest := '__mssql_backup_manifest_<run>', plan := '<run dir>/plan.json');
```

The order matters.

- **The export comes first.** The statement touches no lake catalog entry; it uses only the temp catalog
  and side connections. So DuckLake's transaction, and with it the metadata connection's server
  transaction (`DL/storage/ducklake_transaction.cpp:856-897`), is not open during the hours of writing.
- **The validation opens the lake transaction.** It is the first statement that touches the lake, so
  it runs at the transaction's snapshot S1 and refuses before any DDL or delete (D2.3).
- **The transaction holds** the validation, the DDL, 023's `DELETE` scans and a SWITCH's copy (023 D4,
  D8), then the registration. It runs inside the catalog's server transaction, so its length is the
  `DELETE` scans' and the copy's; a fresh full deletes nothing. A failure in any statement rolls the
  whole transaction back.
- **The apply's summary is the last result** (in autocommit, the commit function's), so it is what the
  PRAGMA returns. The apply also sets the commit message itself, because it alone knows the counts the
  message carries.

**A conflict at commit is retried, bounded.** DuckLake's commit loop retries any attempt that fails
after the conflict check with a retryable error (`can_retry`,
`DL/storage/ducklake_transaction_state.cpp:2665-2707`; `RetryOnError`,
`DL/storage/ducklake_transaction.cpp:1395-1409`); a conflict the check finds never is. An
import's `DELETE`s conflict with a compaction of the same file, or an insert into the table, committed
meanwhile (023 D11). A retry needs a new transaction, which an expansion cannot open. So in autocommit
the expansion ends, after the export, in `ducklake_mssql_backup_commit`: it runs the statements that
follow the export above (validation, DDL, `DELETE`s, a SWITCH's copy, apply) as one transaction on a
side connection, with the run's per-unit manifests in place of the temp table. That transaction is
the import's single snapshot; the caller's transaction then writes nothing to the lake. `CHECKPOINT`
is the precedent for the side connection (`DL/storage/ducklake_checkpoint.cpp:15`), though it runs
each of its steps as its own query (`:26-31`). On a DuckLake conflict the function runs them again from
the validation, up to N = `import.conflict_retries` times (config, default 3), N+1 attempts in all,
then reports the last conflict and the attempts.

- The export is reused: the importer's files are `created_by_ducklake = false` and survive DuckLake's
  `CleanupFiles` (`DL/storage/ducklake_transaction.cpp:76-81`).
- Each retry repeats the `DELETE` scans and the copy.
- Inside an explicit transaction the statements run in the caller's transaction as shown, so a
  conflict fails the caller's COMMIT; running the PRAGMA again resumes from the same export (D9).

**Companion functions** (table functions; usable by hand):

| function | does |
| --- | --- |
| `ducklake_mssql_backup_plan(lake, file, …)` | the callback's work as rows: chain decision, DDL, specs and why, units, bytes, the expansion text. Read-only |
| `ducklake_mssql_backup_export(lake, file, plan := [, max_memory, max_threads])` | the export phase (D2.2); returns the manifest |
| `ducklake_mssql_backup_validate(lake, file, plan :=)` | the re-validation at S1 (D2.3), the transaction's first statement; returns S0 and S1 |
| `ducklake_mssql_backup_apply(lake, file, manifest :=, plan :=)` | the registration (D2.3); returns the summary |
| `ducklake_mssql_backup_commit(lake, file, plan := [, max_memory, max_threads])` | in autocommit, the statements after the export in one transaction on a side connection, a conflict retried (D1), for phase A's registration too (D2.4); returns the summary. `max_threads` is accepted for symmetry and has no effect there (D12) |
| `ducklake_mssql_backup_history(lake)` | imports with snapshot ids, generation, consistency, expired (024 extends it) |
| `ducklake_mssql_backup_cleanup_runs(lake, older_than :=, dry_run :=)` | orphans of unregistered runs (D9) |

### D2. The pipeline: plan, export, register

**D2.1 Plan (the PRAGMA callback, a side connection, the lake at its latest snapshot S0).**

1. `mssql_backup_info([file] + files, position := …)` gives the header (085: the path argument takes the
   list of a media set's families). This entry point takes a full or a copy-only full. On a lake that
   already holds this database it hands over to 023 (`mode := 'auto'`).
2. Lake state at S0: `__mssql_backup.imports` (empty means a fresh lake), `config`, existing schemas. A
   lake that holds another database (a different FamilyGUID, 023 C1) is refused: v1 keeps one source database
   per lake.
3. `mssql_backup_tables` (columns, AUs, `partitioning`), `mssql_backup_views`, `mssql_backup_catalog(file)`
   (with no base table: the list of allow-listed base tables to mirror; then each one's rows),
   `mssql_backup_export_plan(file, …)` (the units, D4).
4. Decide the DDL (D3), partitioning and sort (D5), units, file names and the run id. Write `plan.json`
   into the run directory.
5. The **plan id** is (BackupSetGUID, S0, the latest import's BackupSetGUID or none, S0's schema
   version).

**D2.2 Export (no lake transaction).** `ducklake_mssql_backup_export` is mssql-ducklake's wrapper. It runs
mssql's `mssql_backup_export` (089) on DuckDB's scheduler through a side connection, with the plan's
units and output paths, and with the lake's own layout read at S0:

- `partition_by`: per table, the SQL text of the expression DuckLake's
  `DuckLakePartitionUtils::ApplyPartitionTransform` builds for the table's spec (D5);
- `order_by`: the table's DuckLake sort expressions;
- `target_file_size` and `parquet_options` (compression, version, row group size): the table's or the
  lake's options;
- `max_memory` and `max_threads`: the import's own limits (D12), with the side connection's
  `operator_memory_limit` set to `max_memory` / 2 (D12's split).

The side connection first takes the importing session's `TimeZone` and `Calendar`. They matter only
for a calendar transform (`year` / `month` / `day` / `hour`) on a `TIMESTAMPTZ` column: DuckLake binds
DuckDB's date-part function on the value (`DL/storage/ducklake_partition_data.cpp:263-267`), which for
`TIMESTAMPTZ` is ICU's and reads both settings at bind (`DD/extension/icu/icu-datefunc.cpp:21-26`). The
`epoch_*` transforms use the UTC instant (D5). The plan records both settings for external workers (D4).

The data files come from 089's export. The other kinds are written by the wrapper itself with
`COPY … TO … (FORMAT parquet)` from the reader's functions: the mirrors from
`mssql_backup_catalog(file, t)`, the allocation images from `mssql_backup_allocation` (for a loaded
full, with the export's recovery parameters, D11), and (with 090) the pending set from
`mssql_backup_pending_pages`. It returns one manifest row per file written:

- kind: data, mirror, alloc, or pending;
- target lake table, path, row_count, file_size_bytes, footer_size, row_group_count;
- the column statistics, in the `WRITTEN_FILE_STATISTICS` shape the Parquet writer already produces;
- partition values, unit id, BackupSetGUID, reader version.

Per-unit manifests are also written into the run directory as units finish. That makes the run resumable,
and observable from other sessions (D10). The wrapper never touches the caller's lake transaction.

**D2.3 Validate, then register (one transaction).** `ducklake_mssql_backup_validate` is the first
statement after the export (in autocommit, on `ducklake_mssql_backup_commit`'s side connection, D1). It
runs in `DuckLakeTransaction::Get(context, lake)`, which opens the lake transaction at its snapshot S1,
and re-validates before any DDL or `DELETE`:

- the plan id still holds: no import committed since S0, and this BackupSetGUID is not in `imports`
  (else D9);
- no non-import snapshot since S0 wrote to a table the plan touches (DQ-08), except a DuckLake
  compaction: a snapshot whose only changes to that table are a merge or rewrite of its files
  (`ducklake_merge_adjacent_files`, `ducklake_rewrite_data_files`, a `CHECKPOINT`'s), as 023 D11 / D12
  and DQ-08 allow.

A refusal names the file, the GUIDs and the snapshots. A failure here or in any later statement rolls
the whole transaction back. Then the DDL statements (and 023's `DELETE`s and SWITCH copies) run, and
`ducklake_mssql_backup_apply` registers, in the same transaction:

1. **The manifest**: every manifest file exists with its recorded size, and every planned unit appears
   exactly once; anything else fails, naming the file and the unit.
2. **Data files.** Each manifest file becomes a `DuckLakeDataFile`
   (`DL/include/common/ducklake_data_file.hpp:65-83`), built the way `DuckLakeInsert::AddWrittenFiles`
   builds one (`DL/storage/ducklake_insert.cpp:157-258`):
   - statistics keyed by `FieldIndex` (resolved through `table.GetFieldId`), with the bounds of
     `skip_stats_columns` cleared;
   - `partition_id` taken from the table's spec, and `partition_values` as reported. Where the spec is
     monotone in the column (identity, `year`, `epoch_*`, and `month` / `day` / `hour` beside every
     coarser calendar field of the same column: a `day` beside only `month` wraps every year) each value
     is checked by evaluating DuckLake's own expression, in this session, on the file's min and max of
     the partition column, and a file whose min and max give two values is refused. `bucket` is a hash,
     and a calendar part alone wraps (`month(ts)` puts every January in one value), so min and max prove
     nothing. Under those fields the apply checks each manifest file per row: it evaluates the bound
     transform over the file's partition column, a one-column scan that refuses any row whose value
     disagrees. The export's own per-row check stays (089 `partition_by`), and files from external
     workers (D4) are verified too;
   - `mapping_id`, a name map through `transaction.AddNameMap`, unless the files carry field ids
     (DQ-24);
   - `created_by_ducklake = false`, so a failed commit keeps the file for a retry
     (`DL/storage/ducklake_transaction.cpp:76-81`).

   Then `transaction.AppendFiles(table_id, files)` per table. The table ids are the transaction-local ids of
   the tables the DDL just created, found through the catalog.
3. **The importer's own rows**: provenance, registry, views. The apply writes them as small Parquet files
   through the Parquet copy function (as `DuckLakeDeleteFileWriter` writes its files,
   `DL/storage/ducklake_delete.cpp:38-155`) and registers them the same way. Everything enters through
   `AppendFiles`, and there are no native inserts to inline.
4. `transaction.SetCommitMessage(...)` (D8).
5. **The summary**: one row per table (schema, table, action, rows, files, bytes, consistency), plus a
   total row.

The query ends; DuckLake's `Commit()` → `FlushChanges()` writes the snapshot (D7).

**D2.4 Phase A, the v1 pipeline (before 089's export; the fallback and the oracle).** The same plan,
expanded into DuckDB SQL only (as it runs inside an explicit transaction; in autocommit, below):

```sql
COPY (SELECT *, _mssql_rid, _mssql_page,   -- virtual columns are not in *; add _mssql_lob for a
                                            -- table with off-row-capable columns (085 PR 3, DQ-05)
               <partition_by> AS epoch_month  -- DuckLake's bound expression for the spec, as SQL (D5)
        FROM mssql_backup_scan('…', 'dbo', 'orders'))
  TO '<data_path>/dbo/orders/'
  (FORMAT parquet, PARTITION_BY (epoch_month), FILE_SIZE_BYTES '<the lake''s target_file_size>',
   FILENAME_PATTERN 'mssql-<guid>-<run>-o1029578706-{i}');
-- … per table, per mirror (mssql_backup_catalog(file, 'sysschobjs')) and for the allocation images
--   (mssql_backup_allocation(file)) …
SELECT * FROM ducklake_mssql_backup_validate('lake', '…', plan := '<run dir>/plan.json');  -- S1 (D2.3)
CREATE SCHEMA …;  CREATE TABLE … WITH (…);
CALL ducklake_add_data_files('lake', 'orders', '<data_path>/dbo/orders/**/mssql-<guid>-<run>-*.parquet',
                             schema := 'dbo', hive_partitioning := true);
INSERT INTO lake.__mssql_backup.imports VALUES (…);  -- … tables, columns, views, config
CALL ducklake_set_commit_message('lake', 'mssql-ducklake', 'full sales_full.bak', extra_info := '{…}');
```

- `ducklake_add_data_files` maps columns by name, reads statistics from the footers, appends to the
  caller's transaction, and is not rolled back on failure (research-ducklake-import §2).
- **The same bounded retry as the apply.** In autocommit the statements after the `COPY`s
  (validation, DDL, 023's `DELETE`s and a SWITCH's copy, `ducklake_add_data_files`, the provenance
  rows, the commit message) run through `ducklake_mssql_backup_commit`, so a conflict at commit is
  retried up to N = `import.conflict_retries` times on the same files (D1). A failed attempt keeps
  them: `ducklake_add_data_files` registers its files with `created_by_ducklake = false`
  (`DL/functions/ducklake_add_data_files.cpp:471`), which `CleanupFiles` leaves in place.
- Nothing records which file or position a row went to. A later file deletes rows by the value of
  `_mssql_page` (023 D4), so correctness never depends on where rotation put a page's rows.
- Partition values come only from Hive paths (`DL/functions/ducklake_add_data_files.cpp:1084-1120`). The
  `COPY` computes them per row: it binds DuckLake's `ApplyPartitionTransform` for the table's spec in the
  importing session, renders the bound expression to SQL (the text 089's `partition_by` takes, D2.2;
  the rendering is to verify, task 4) and evaluates that. It matches DuckLake for every type, `_NS` and
  TZ included, and for every transform, overrides included: a `bucket` field renders to `murmur3_32`,
  which DuckLake registers as a SQL function (`DL/ducklake_extension.cpp:124-126`). No transform is
  written by hand. Each row lands under its own value, so a phase-A file holds one value by
  construction.
- `_mssql_lob`: 085 PR 3 carries its plain output, the root bytes and the node list (DQ-05, accepted),
  so a phase-A lake has its LOB maps from the first import; 089 PR 1 adds the hybrid walk.
- Phase A runs the tables one `COPY` after another, each `COPY` parallel inside, in the order DuckDB's
  COPY gives; correctness never depends on it, and the files follow DuckLake's partition values and
  file size like DuckLake's own INSERT.
- Phase B must produce the same lake as phase A, up to file names and file boundaries. A lake-vs-lake
  comparison keeps the two honest (Testing).

### D3. The lake a full import creates

**User tables**: the table's columns, then the service columns.

| service column | type | content |
| --- | --- | --- |
| `_mssql_rid` | BLOB | the reader's row locator (RESEARCH §5), led by its DQ-02 shape byte: a heap's original location `0x00 ‖ u32 page ‖ u16 file ‖ u16 slot` (`%%physloc%%` after the shape byte, following the forwarded back pointer); a clustered table's `0x01 ‖` key + uniquifier; a CCI's `0x02 ‖ u64 rowset_id ‖ u32 rowgroup ‖ u32 tuple`, a delta-store row's `0x03 ‖ u64 rowset_id ‖ u32 rowgroup ‖` hidden key (088 D3). Opaque, and encoded by 085 |
| `_mssql_page` | VARCHAR | the page the row lives on now, as the uppercase hex of its shape-byte encoding, fixed width per shape: a page is `01` + 4 hex digits of file id + 8 of page id, 14 characters (the `01` form; a delta-store row's delta page too); a compressed columnstore row, which has no page, has its rowgroup key, `02` + 16 hex digits of rowset id + 8 of rowgroup, 26 characters (the `02` form; 088 D3). Hex digit order is byte order, so within a shape string order is page order, and a file's min/max on it prunes DuckLake's delete. The value 023 deletes by (023 D4) |
| `_mssql_lob` | BLOB | only on tables with off-row-capable columns (LOB types, row-overflow). Opaque bytes serialized by the C++ code (DQ-02, decided): per off-row value its SQL Server column id (a rename changes nothing), the root bytes, the fragment map (E1 rule 7, E2 R8) and, per DQ-01's hybrid, the raw old bytes only where the lake value does not re-encode exactly (an unpaired surrogate in UTF-16, an unmapped code-page byte). Raw bytes for `xml` / `json` / CLR values are kept only once a fixture shows that a partial update leaves their fragments out of a differential; until then a splice over one is refused by name (089 D6). The nodes keep a binary page: only the C++ code reads it, and 089 compares the decoded page when it looks a node up against changed pages |

**Why `_mssql_page` is VARCHAR** (owner, 2026-11-06). DuckLake builds no file filter for a BLOB column:
`GenerateFilterFromExpression`'s constant filter returns nothing for `LogicalTypeId::BLOB`
(`DL/storage/ducklake_metadata_manager.cpp:1764-1765`). mssql-ducklake measured it on 5 files, each
holding a distinct page range: a BLOB `=` / `IN` / `IN (SELECT …)` / `BETWEEN` read 5 of 5 files; the
same values as BIGINT read 1 of 5, and as hex VARCHAR 1 of 3; HUGEINT is not pruned at all. Not BIGINT:
a rowgroup key is 96 bits, and a second column was refused (DQ-02). `_mssql_rid` and `_mssql_lob` stay
BLOB, since nothing prunes on them: a delete-bitmap delete is `_mssql_page = <key> AND _mssql_rid IN
(…)`, and the page term prunes the files (023 D9). Every place that carries a page uses the same VARCHAR
form: `mssql_backup_changed_pages.page`, `pending_pages.page`, `stale_pages.page`,
`delete_pages.parquet` and the moved-page lists (023 D4, D8); `alloc_pages` keeps `file_id` /
`page_id` (089's schema).

DuckLake runs its file filter as string comparisons in the metadata database
(`DL/storage/ducklake_metadata_manager.cpp:1693-1745`). On a SQL Server catalog this extension stores
every VARCHAR column, the min/max statistics columns included, as `COLLATE
Latin1_General_100_BIN2_UTF8`, and its comment says this is what makes DuckLake's pushed min/max prune
correctly (`MD/include/mssql_metadata_manager.hpp:165-168`, `MD/mssql_metadata_manager.cpp:161`,
`MD/mssql_catalog_shape.cpp:833-847`). BIN2 compares by code point, so the fixed-width uppercase hex
orders as bytes there; a test keeps it (task 11).

No data page's LSN is stored, with the rows or in a list of pages; the only page LSNs in the lake are
the allocation images' and the pending images' own (`alloc_pages.page_lsn`, `pending_pages.lsn`). Which
pages changed is decided by the next file (its DCM and `m_lsn > W`, 023 D3) plus `pending_pages` and
`stale_pages`.

Types are the reader's: 085 D6's map, which is the live `TYPE mssql` catalog's, with 086's exceptions.
What they mean for the lake:

- strings are plain `VARCHAR`: DuckLake refuses alias and collated types (`DL/common/ducklake_types.cpp:158-184`).
  The declared length, collation and code page are kept in `__mssql_backup.columns`;
- `datetimeoffset` → `TIMESTAMPTZ` drops the offset, as the live mssql catalog does; the instant is
  exact (085 D6);
- `time(7)` → `TIME`, in microseconds, the 100 ns digit truncated: `TIME_NS` would keep it, but on the
  2.0 pin it neither compares with `TIME` nor takes INTERVAL / strftime / date_trunc;
- `datetimeoffset(7)` → `TIMESTAMPTZ`, in microseconds, the 100 ns digit truncated: the live catalog
  reads every `datetimeoffset`, whatever its scale, as the µs `TIMESTAMPTZ`, and 085 D6 takes its map.
  DuckLake has `time_ns` / `timestamptz_ns` (`DL/common/ducklake_types.cpp:34,42`), but the reader's
  types decide (085 D6);
- `datetime2(7)` → `TIMESTAMP_NS` (085 D6);
- `vector` → `LIST(FLOAT)`: 086 reads `FLOAT[n]` (DQ-19), and DuckLake has no ARRAY (none in its type
  list or `ToString`, `DL/common/ducklake_types.cpp:19-52,158-189`), so the importer casts to `FLOAT[]`;
- `sql_variant` → `VARIANT` (086, DQ-19); geometry and geography → `GEOMETRY` (WKB from 086); `json` →
  `JSON`; `xml` → `VARCHAR`;
- identity columns become plain columns (seed and increment in the registry);
- persisted computed columns become plain columns. Non-persisted ones are not in the lake: DuckLake
  refuses generated columns, so their text goes into the registry;
- every lake column is nullable (DQ-23).

**Table options, at CREATE**: `WITH (data_inlining_row_limit = 0, skip_stats_columns = [...])`. The
skipped statistics are `_mssql_rid`, `_mssql_lob` and every `(max)` column. Their bounds prune nothing,
and a min or max past the catalog's `stats_length` turns the appender off for the whole commit
(specs/018, `MD/mssql_metadata_manager.cpp:26-31`). `_mssql_page` is never skipped: its min/max per file
is what a delete by page prunes by (023 D4; one check remains, task 11), and its 26 characters
at most fit far under `stats_length`, 1,024 bytes by default (specs/018, § The options). Why per table
and not lake-wide: D6.

**`__mssql_sys`** (the physical state the next file needs; all of it created by the first import):

| table | content |
| --- | --- |
| one mirror per base table `mssql_backup_catalog(file)` lists (085's allow-list) | the raw rows of `sysschobjs`, `syscolpars`, `sysrowsets`, `sysallocunits`, `sysrscols`, `sysidxstats`, `sysiscols`, `sysseobjvalues`, `sysclsobjs`, the columnstore tables (`syscsrowgroups`, `syscscolsegments`, `syscsdictionaries`), E7's partitioning tables (`systypedsubobjs`; `syssingleobjrefs` classes 7, 8, 30, 31), and `sysobjvalues` (rows for view definitions, partition boundaries (valclass 30), and computed-column and default definitions, only: not module source; DQ-11). Each has its own `_mssql_rid`, `_mssql_page` (E1 rule 6) |
| `alloc_pages` | `(file_id, page_id, page_type, page_lsn DECIMAL(25,0), au_id, image BLOB)`, the schema of 089's `allocation.parquet`, which 023 writes from it and the reader checks exactly: `mssql_backup_allocation`'s rows (for a loaded full, those of its recovered state, D11), the latest image of every allocation page (file header, GAM, SGAM, PFS, DCM, BCM, IAM; E2 R5), a side input to 089. The IAM images on each AU's first-IAM chain in the mirror's `sysallocunits` name the allocation unit that owned each page (an extent, or a mixed extent's single-page slot) in the file the lake reflects. The reader (089) maps that AU to its table through the base mirror and reports it as `from_object_id` / `from_partition_number`; the importer maps those to a lake table through the registry: that is how 023 knows which table a page's rows are deleted from (023 D4). They record the AU, not the table, because a SWITCH changes the table and leaves the AU (E7§4). 023 replaces images file by file (023 D3 step 7); a full replaces the whole set |
| `pending_pages` | `(file_id, page_id, page, xids UBIGINT[], lsn, image BLOB, import_seq)`, `page` in `_mssql_page`'s VARCHAR form: `mssql_backup_pending_pages`'s rows (090) plus the import that produced them; filled by a loaded full (D11); passed back as `base/pending.parquet` and resolved by 023. They hold uncommitted rows: DQ-09 |
| `stale_pages` | `(page, reason, import_seq, skipped_xids UBIGINT[])`, `page` in `_mssql_page`'s form, `reason` as 090 reports it (`baseless`, `skipped_transaction`, `lob`): created empty; 023 fills it |

LSNs are `DECIMAL(25,0)` in SQL Server's numeric form (vlf · 10¹⁵ + block · 10⁵ + slot), which orders the
way LSNs order and is the form HEADERONLY prints.

**`__mssql_backup`** (the importer's own state):

| table | content |
| --- | --- |
| `imports` | one row per applied file (D8) |
| `tables` | one row per imported object: object_id, the SQL Server schema and name, shape (heap / clustered / CCI), the lake schema and table it lives in, the import that created it, `created_partitioning`, `created_sorting`, `spec_source` (override / derived / none) and why, status (live / dropped) |
| `columns` | object_id, colid → the lake column, SQL Server type text, declared length, collation, nullability, default text, computed text, identity seed and increment |
| `views` | object_id, schema, name, the T-SQL definition (from `mssql_backup_views`) |
| `config` | key/value: the filters, the retention policy (024), the loaded-diff mode (023), `import.run_path` (D9), `import.conflict_retries` (D1), `import.max_memory` and `import.max_threads` (D12) |

**Sizes** (estimates, except where measured):

| piece | size | source |
| --- | --- | --- |
| `alloc_pages` | about 0.02 % of the database: a PFS page per 64 MB, GAM/SGAM/DCM/BCM per 4 GB, IAMs | E2 R5 |
| `_mssql_lob` | about 30 B per 8 KB LOB leaf | E1 §6 |
| mirrors | ~6,300 rows for E1's small database; grows with objects × columns | E1 §3 |
| `pending_pages` | ∝ pages that open transactions touched: 3,372 pages, 12 % of a 240 MB database, in E6's loaded full | E6 §3.2 |

### D4. Parallel writers, and what the importer needs from 089

**Units**, from `mssql_backup_export_plan`. A unit is (object, index partition rowset, range), sized near
the table's DuckLake `target_file_size`. SQL Server partitions are **work units only**: the files follow
DuckLake's layout (D5).

- clustered rowstore: disjoint key ranges cut at level-1 index pages, read in leaf-chain order, which gives
  key order and page contiguity at once;
- heap: IAM extent ranges in ascending page order, so a file's `_mssql_page` min/max is narrow too;
- clustered columnstore: rowgroups in tuple order, with delta stores as pages.

Units run in parallel on DuckDB's scheduler, capped below, so a big table does not wait behind another,
as it would in `COPY FROM DATABASE`.

**The writer contract** (research-ducklake-import §5, 089 § Surface), which 089 honours:

- one thread per open file; in physical order a page's rows are emitted together, so a file holds a
  contiguous range of pages and its `_mssql_page` min/max is narrow, which is what a delete by page
  prunes by (023 D4);
- a page's rows are split over files only at a rotation or at a change of DuckLake partition value.
  When the table's DuckLake sort order is not satisfied by the physical order, the unit is sorted first
  and a page's rows may scatter over many files (correct, but a later delete of that page is expected to
  read more files, 023 D4; DQ-25);
- each file reports its row count and column statistics, `_mssql_page`'s min and max included;
- one open file per partition value per unit;
- memory is one row group per open file, sized in bytes (below), plus the sort's spill when a sort is
  needed;
- **the cap is the reader's** (089), and row groups are sized in bytes, not rows. With B = the lake's
  row group in bytes (`parquet_row_group_size_bytes`, else `parquet_row_group_size` rows × the table's
  estimated row width; `DL/functions/ducklake_options.cpp:22-23`), the export keeps at most
  F = max(1, ⌊`max_memory` / (4 · B)⌋) files open, runs at most min(`max_threads`, F) units at once,
  and writes with a Parquet `ROW_GROUP_SIZE_BYTES` ≤ `max_memory` / (4 · F), less one chunk (089). A
  row wider than the estimate makes a smaller row group, never more memory; a row that cannot fit
  even alone (a LOB value of up to 2^31 − 1 bytes) is refused by name (D12's rule for a failed
  reservation). A unit that needs a file while F are open first closes its own least recently written
  one (more, smaller files, never more memory), and one that holds none itself waits until another
  unit closes one. The bound: the writers' buffers stay under their share, a quarter of `max_memory`
  (D12's split), whatever the partition spec and the rows' widths. A sort is DuckDB's, spilling, under
  `memory_limit` and the side connection's `operator_memory_limit`, `max_memory` / 2 (D12). A unit's
  open files are its partition values: one or two under a spec proposed from SQL Server's
  partitioning, up to n under `bucket(n)`. The importer passes `max_memory` and `max_threads` through
  (D12); a memory test checks the bound (Testing).

**File names**: `<table data path>/mssql-<BackupSetGUID>-<run8>-o<object_id>-p<partition>-u<unit>-<seq>.parquet`
(passed to 089 as `naming :=`). They are deterministic within a run, which is what resume relies on.
The run id keeps two concurrent attempts on one lake from writing the same object. The owner asked for
names keyed by BackupSetGUID / table / chunk, which these are, and the run id stays (DQ-34, accepted).
`p<partition>` names the work unit's SQL Server partition only; it says nothing about the file's DuckLake
partition value.

**External workers.** `ducklake_mssql_backup_plan` writes the plan. Other processes or machines run
`mssql_backup_export(file, out := …, units := [...], …)` with the arguments `plan.json` lists
(`since_lsn`, `base`, `partition_by`, `order_by`, `target_file_size`, `parquet_options`, `naming`), in a
session set to the plan's `TimeZone` and `Calendar` (D2.2), with write access to the data path and the
run directory only (they never touch the lake catalog), and each drops `manifest-<units>.parquet` (the
export's result rows) into the run directory. Then
`PRAGMA ducklake_import_mssql_backup(…, manifests := '<run dir>/manifest-*.parquet')` validates and
registers. Within an import a side connection writes files only, never lake rows
(research-ducklake-import §8), with exactly one exception: in autocommit, the transaction of
`ducklake_mssql_backup_commit` on its side connection is the import's single snapshot, and the
caller's transaction then writes nothing to the lake (D1). `CHECKPOINT` is the precedent for a side
connection (`DL/storage/ducklake_checkpoint.cpp:15`).

**Side inputs and outputs.** A fresh full needs no side input. For a loaded full, the wrapper writes the
pending set from `mssql_backup_pending_pages` (D11). 023 adds the side inputs under the run's `base/`
(catalog, allocation, LOB, pending; 089 D10).

### D5. Partitioning and sort: proposed once, at creation; DuckLake's afterwards

**Owner decision (2026-11-05, DQ-10): follow DuckLake's rules, do not replicate SQL Server.** Once the data
has left SQL Server, its physical placement is DuckLake's business. SQL Server's partitioning and
clustered key only *propose* the spec a lake table is created with; the partition values, the file layout,
the sort order and the file size are DuckLake's, exactly as for any DuckLake writer.

Per table, when the lake table is created (at this import, or when a table first appears in a later file,
023):

1. **An override from the call wins**: `partitioning := MAP {'s.t': '<spec>' | 'none'}`, and likewise
   `sorting`. The plan refuses an override whose column does not exist or whose transform does not apply
   to the column's lake type. A `bucket(n, col)` override is allowed, with a warning naming its cost: it
   scatters a page's rows over n files, so every later delete of that page is expected to read n files
   (023 D4; DQ-26). A bucket spec a user sets later with DuckLake's own ALTER is written like any other,
   since manual changes win.
2. **The proposal from SQL Server's partitioning** (085's `partitioning`, from E7; the rule held on every
   row of E7's 19 tables, E7§3):
   - an inner partition is the closed range `[b(i−1), b(i) − tick]` under RANGE RIGHT and
     `[b(i−1) + tick, b(i)]` under RANGE LEFT; the tick is the parameter type's own (1, 1 day, 10^−s s,
     1/300 s);
   - the proposal is the finest T of identity < `epoch_day` < `epoch_month` < `epoch_year` with T(low) =
     T(high) for every inner partition (the endpoints suffice: T is monotone). Monthly partitions give
     `epoch_month`, quarterly ones `epoch_year`, consecutive integers identity, daily `date` partitions
     identity (= `epoch_day` for a date);
   - no inner partition (0 or 1 boundary): no spec. Non-temporal types: identity only. Strings: never. A
     misaligned function (RANGE LEFT with period starts, boundaries crossing a year) gets no spec;
   - the rule uses the exact SQL Server values, since it describes the data's partitioning (E7§3); for a
     `datetimeoffset` column, the boundaries' UTC instants, as SQL Server defines its partitions (E7:
     `t_dto_r` → `epoch_year`), and as DuckLake computes an `epoch_*` value at the pin (step 3). It is
     only a proposal; the values are step 3's;
   - if the average partition holds less than a quarter of `target_file_size` (estimated from
     `rcrows`), T is coarsened, so tiny daily partitions do not become tiny files;
   - `epoch_hour` is not proposed (E7's verified rule stops at `epoch_day`). The `epoch_*` transforms are
     format 1.1 (`DL/include/storage/ducklake_partition_data.hpp:20-31`); DuckLake has no truncate
     transform.
3. **The partition values are DuckLake's.** Every row's value is computed with DuckLake's own
   `DuckLakePartitionUtils::ApplyPartitionTransform` for the table's current spec
   (`DL/storage/ducklake_partition_data.cpp:257-278`), bound in the importing session as for any DuckLake
   writer and rendered to SQL: 089's `partition_by` in phase B, the `COPY`'s expression in phase A
   (D2.4). Nothing is written by hand, so the values follow DuckLake for every type. At the pin
   (`DL/storage/ducklake_partition_data.cpp:224-255`):
   - `epoch_*` on TIMESTAMPTZ is computed on the UTC instant, whatever the session's `TimeZone`
     (`ducklake/test/sql/partitioning/epoch_timestamptz.test`), so a `datetimeoffset` column's values
     agree with step 2's UTC proposal;
   - TIMESTAMP_NS (`datetime2(7)`) and TIMESTAMP_TZ_NS are floored to microseconds
     (`FloorNanosToMicros`) before an `epoch_*` transform, not rounded, so a value in the last 500 ns of
     a period keeps its period (`ducklake/test/sql/partitioning/epoch_transforms.test:192`);
   - the calendar transforms (`year` / `month` / `day` / `hour`; an override or a user's ALTER, since
     step 2 proposes none) bind DuckDB's date-part function on the value itself (`:263-267`). On
     TIMESTAMPTZ that is ICU's, in the session's `TimeZone` and `Calendar` (D2.2). On TIMESTAMP_NS it
     takes the implicit cast to microseconds, which rounds: `year` / `month` / `day` exist only for
     DATE / TIMESTAMP / INTERVAL
     (`DD/extension/core_functions/scalar/date/date_part.cpp:2177-2192,2461-2473`; `hour`
     `:2202-2229,2669-2671`), TIMESTAMP_NS casts implicitly only to TIMESTAMP
     (`DD/src/function/cast_rules.cpp:281-289`), and that cast rounds
     (`DD/src/common/operator/cast_operators.cpp:1118-1146,1497-1499`). This is the only case where
     the importing session's TimeZone decides a value, and the docs say so.

   E7§3 measured DuckDB's own `date_diff` / `month()`, not DuckLake's transform, and so missed DuckLake's
   normalisation, which `epoch_*` has had since it arrived (a56fafd4, 2026-08-12); task 4 re-runs it
   through DuckLake's transform at the pin. The importer never declares a value DuckLake would compute
   differently. At this DuckLake pin only `bucket` values prune files
   (`DL/storage/ducklake_metadata_manager.cpp:2183-2258`), so temporal values shape grouping and
   compaction, not results; temporal pruning comes from min/max statistics.
4. **The sort order, proposed** (DQ-25): the clustered rowstore key, in `sysiscols` order and direction,
   becomes `SORTED BY (k1 [DESC], …)`, **only when every key column orders in DuckDB as it does in SQL
   Server**: mssql's `OrdersLikeDuckDB` allow-list (numeric, bit, date/time). The physical leaf-chain
   order then already satisfies the sort, so nothing is sorted and a page's rows stay together. Nothing
   else is proposed, and the reason is recorded:
   - a string key under a collation: a collation's order is not DuckDB's, so the physical order never
     satisfies such a sort;
   - an ordered CCI's `ORDER (col)`: it does not hold row by row inside a rowgroup (VertiPaq reorders the
     tuples), so the export would sort every rowgroup and spread its rows over files, where its rowgroup
     key otherwise finds them in few;
   - a heap, which has no key.

   The reason is the cost of a delete: the export writes in the table's DuckLake sort order (089
   `order_by`) and DuckLake's compaction re-sorts by it
   (`DL/functions/ducklake_compaction_functions.cpp:718-726`), which spreads the rows the same way. A sort
   the physical order does not satisfy scatters a page's or a rowgroup's rows over many files, and every
   later `DELETE … WHERE _mssql_page IN (…)` is then expected to read more of the table's files (023 D4):
   a cost, never a wrong row. A user can still declare any sort with DuckLake's own ALTER, at that cost.
5. **The file layout is DuckLake's**: files are split by DuckLake partition value (a DuckLake file has one
   partition value set), written in the DuckLake sort order and cut at DuckLake's `target_file_size`.
   They are never split per SQL Server partition; those are the export's work units only (D4). A page
   whose rows have several DuckLake values has rows in one file per value.
6. The decision and its source are recorded in `__mssql_backup.tables`; E7's derivations are the test
   expectations (§ Testing).

**Afterwards the spec is DuckLake's.** Manual DuckLake changes win, and later imports write under
whatever spec the table has then. The importer changes a spec in one case only (023 D5, D8):

- When the backup's catalog removes a column a partition or sort field uses, by a DROP COLUMN or by a
  type change done as ADD / DROP / RENAME, the table stays, so the importer removes only the field the
  change hits; the rest of the spec, manual edits included, stays. DuckLake refuses to drop a column a
  field uses (`DL/storage/ducklake_table_entry.cpp:876-900`).
- The removal is recorded in the commit message and the import's provenance (`imports`, `tables`), and
  a WARNING names it.
- A SQL Server re-create (DROP + CREATE of a table) re-creates the lake table, and the new lake table
  gets the creation-time proposal; the old table's manual spec edits go with the old table.
- A type promotion DuckLake applies in place (`ALTER COLUMN … TYPE`) keeps its fields. Under a `bucket`
  field whose hash changes with the type (`BucketChangesWithType`,
  `DL/storage/ducklake_partition_data.cpp:220-222`), DuckLake itself starts a new partition spec, and the
  older files keep their buckets under the old one (`DL/storage/ducklake_table_entry.cpp:1143-1156`).

### D6. Inlining off; standard compaction

**A table option, not a lake option.** Every table the importer creates, mirrors and side tables
included, gets `data_inlining_row_limit = 0` in its `CREATE … WITH (…)`. Table options are accepted at
CREATE (`DL/storage/ducklake_table_entry.cpp:1378-1405`, scope checked by
`DL/common/ducklake_util.cpp:780-788`). `ducklake_set_option` would refuse a table or schema that is
transaction-local (`DL/functions/ducklake_set_option.cpp:96-105`), and these all are. The lake-wide
default stays as the user set it, so the user's own tables keep inlining.

**Why inlining 0.** `AppendFiles` never inlines, but some writes inside an import are native:

- phase A's provenance inserts, and 023's `DELETE`s by `_mssql_page` and a SWITCH's `INSERT … SELECT`
  (023 D4, D8);
- a delete of up to `limit` rows would become an inlined file deletion
  (`DL/storage/ducklake_delete.cpp:523-537`), rows in the catalog database, instead of a delete file
  beside the data files it deletes from.

**Compaction is DuckLake's.** Imported tables are compacted like any DuckLake table: `CHECKPOINT`,
`ducklake_merge_adjacent_files` and `ducklake_rewrite_data_files` run on them under the lake's or the
table's own options (`target_file_size`, `rewrite_delete_threshold`). The importer has nothing to do
after a compaction: nothing it keeps refers to a file or a row position, and every delete finds its rows
by the value of `_mssql_page` (023 D4), wherever a compaction moved them. A compaction is another
writer's snapshot: committed before the import's transaction, it changes nothing; committed during it,
it conflicts when it merged or rewrote a file the import deletes from. In autocommit the import's
retry, bounded, runs the post-export part again on the same export; inside an explicit transaction the
COMMIT fails and a re-run resumes from the export (D1, 023 D11). That is the likeliest collision,
because a compaction merges the small files earlier differentials left, which are the next import's
delete targets. The docs recommend running maintenance (`CHECKPOINT`, the compaction functions) between
imports.

When to merge the small files that differentials leave, or rewrite the files whose deletes keep growing,
is the lake's own choice, as for any table. `merge_adjacent` keeps row order when the files are adjacent
and no DuckDB-dialect sort applies (`DL/functions/ducklake_compaction_functions.cpp:727-745`), so a
page's rows stay together; D5's sort rule keeps it that way. What a delete by page reads after a merge
is measured, not assumed (023 task 6). Retention does not compact; an expiry after a compaction frees the
files it replaced (024 §D5).

### D7. The commit, concretely

**DuckLake calls made by the apply** (classes of the DuckLake compiled into this extension; the sources
are never edited). They need the embedded image (spec 002): a stock DuckLake loadable is built with
hidden symbols, so `AppendFiles`, `AddNameMap`, `SetCommitMessage` and the apply's mirror of
`AddWrittenFiles` (D2.3) are reachable only from inside it. A test checks that phase B writes the same
metadata rows as `ducklake_add_data_files` for the same files (Testing).

| call | for |
| --- | --- |
| `DuckLakeTransaction::Get(context, catalog)` | the caller's lake transaction (`DL/include/storage/ducklake_transaction.hpp:223`) |
| `AppendFiles(table_id, vector<DuckLakeDataFile>)` | every data, mirror, side-table and provenance file (`:248`) |
| `AddNameMap(…)` | name-mapped files, unless they carry field ids (`:254`) |
| `SetCommitMessage(DuckLakeSnapshotCommit)` | author, message, extra_info (`:300`) |

023's deletes need no call of their own: they are DuckLake's `DELETE` statements in the expansion, run
before the apply registers the new files (023 D4).

**What DuckLake writes at commit.** `Commit()` → `FlushChanges()` → the commit loop
(`DL/storage/ducklake_transaction_state.cpp:2651-2738`). Each attempt increments `snapshot_id` once and
sends one batch:

- `ducklake_snapshot` and `ducklake_snapshot_changes` (`changes_made` lists every table touched);
- `ducklake_schema_versions`; `ducklake_schema`, `ducklake_table` and `ducklake_column` rows;
- `ducklake_partition_info` / `_column` and `ducklake_sort_info` / `_expression`;
- `ducklake_metadata` rows for the table-scoped options;
- `ducklake_column_mapping` / `ducklake_name_mapping` for name maps;
- `ducklake_data_file` (with `row_id_start` from the table's `next_row_id`), `ducklake_file_column_stats`,
  `ducklake_file_partition_value`;
- `ducklake_table_stats` and `ducklake_table_column_stats`.

**How it reaches SQL Server**:

- `MSSQLMetadataManager::Execute` recognises each statement from spec 014's closed list and sends it as
  the manager's T-SQL: runs through `mssql_exec` on the pinned connection, `VALUES` in chunks of 1,000
  rows (error 10738).
- The appender (specs 006 and 014 D5; BCP on the 2.0 line) carries the data-file rows, file statistics and
  partition values.
- It is all one server transaction.
- The server-side apply (spec 005) does not apply: it covers data-files-only commits
  (`MD/mssql_server_commit.cpp:258-262`), and an import has DDL and name maps.
- The suite runs with `MSSQL_DUCKLAKE_STRICT_BATCH=1`. A shape the closed list lacks fails it by name;
  name-map inserts from an add-files commit are the likely one, so add them.

**Metadata volume.** Let T = tables created, C = columns per table including the service columns, F = data
files, P = mirror, allocation, pending and provenance files. Then:

- rows ≈ T·(2C + 6) for DDL and stats, + F·(1 + C) for files and their column statistics, + P·8, + (name
  maps) T·(1 + C), + a handful.

| scenario | T | C | F | P | rows (with name maps) | path |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| A: 1 TB, 2,000 tables, full | 2,000 + ~15 | 43 | ~4,000 | ~25 | ≈ 360 k (≈ 450 k) | ~185 k DDL rows as ~185 `VALUES` statements in a few `mssql_exec` batches; ~175 k file rows as bulk loads |
| B: 50 GB, 200 tables | 215 | 43 | ~300 | ~25 | ≈ 33 k (≈ 43 k) | seconds |
| C: one 2 TB table, 512 MB files | 16 | 30 | ~4,000 | ~25 | ≈ 125 k | mostly appender rows |

Research-ducklake-import's "~80 k rows for 2,000 files × 40 columns" counts files only. The DDL of 2,000
wide tables more than doubles it. **Not measured** (E8 was not run): the budget is ≤ 30 s for scenario A
against a local server, and task 12 measures it. The commit holds the catalog's snapshot lock for its
duration, so other writers to the lake wait. A full import is expected while the lake is otherwise quiet;
023's differentials are small (023 §D11). If the commit proves slow, the fix is the one research §4.2
names: extend the server-side apply to DuckLake's staged `DELETE_FILE` / `DROPPED_FILE` / `NAME_MAP`
tables (`DL/include/storage/ducklake_staged_commit.hpp:39-58`).

**Retries.**

- The loop retries any attempt that fails after the conflict check with a retryable error
  (`can_retry`, `DL/storage/ducklake_transaction_state.cpp:2665-2707`; `RetryOnError`,
  `DL/storage/ducklake_transaction.cpp:1395-1409`); a conflict the check finds never is. The importer
  retries that itself, bounded, in autocommit (D1).
- The importer's files survive every attempt (`created_by_ducklake = false`).
- A final failure runs `CleanupFiles()`, which removes only DuckLake's own files. The run stays resumable
  (D9).

**Any embedded backend.** Only DuckLake's transaction is used, so the import works on every metadata
backend the embedded DuckLake serves: DuckDB file, SQLite, Postgres and mssql. The no-server test lane
uses a DuckDB-file lake.

### D8. Provenance and the commit message

**`__mssql_backup.imports`**, one row per applied file:

- `import_seq`, `run`, `mode` (fresh / incremental / rebase), `generation` (024);
- `backup_set_guid`, `backup_type` (full / copy_only_full / diff), database name, `family_guid`,
  `recovery_fork_id` (HEADERONLY's RecoveryForkID, which 085 task 3 locates; NULL until then). A fork
  the file does not yield, or the lake did not record, refuses every apply by LSN, as 089 D7 states;
  the way out is a rebase on a full, which records the fork once 085 reads it (023 C1);
- `first_lsn`, `checkpoint_lsn`, `last_lsn`, `database_backup_lsn`, `differential_base_guid`,
  `differential_base_lsn`, and the watermark `w = min(first_lsn, checkpoint_lsn)` (E1 rule 2, E6);
- `backup_start_date`, `backup_finish_date`;
- the files with their size and ETag, compression, the encryption thumbprint (never key material);
- `consistency` (consistent / pending / stale / dirty), pending and stale page counts;
- counts: pages, rows, files and tables created / altered / dropped;
- the mssql and mssql-ducklake versions, and timings.

The row's snapshot is its DuckLake `snapshot_id` virtual column
(`test/sql/virtualcolumns/ducklake_snapshot_id.test`), which still answers after the snapshot expires.

**Commit message**: author `mssql-ducklake`, message `<type> <file basename>`, and
`extra_info = {"kind": "mssql_backup_import", "v": 1, "backup_set_guid", "type", "database",
"family_guid", "first_lsn", "checkpoint_lsn", "last_lsn", "differential_base_guid",
"differential_base_lsn", "backup_finish_date", "generation", "consistency", "run"}`.
`ducklake_snapshots()` shows it until the snapshot expires; `imports` keeps it for good.

### D9. Failure, retry and orphans

- **Nothing is visible before the commit.**
- **The run directory** `<run_path>/<BackupSetGUID>/<run>/` holds `plan.json`, the per-unit manifests,
  023's side inputs (`base/`, `delete_pages.parquet`, the moved-page lists) and a status file. It lives
  **outside DATA_PATH**:
  - `CHECKPOINT` runs `ducklake_delete_orphaned_files` (`DL/storage/ducklake_checkpoint.cpp:21-24`),
    which deletes every unregistered `.parquet` / `.puffin` under DATA_PATH
    (`DL/storage/ducklake_metadata_manager.cpp:5290-5294`) older than `delete_older_than`, 2 days by
    default (`DL/functions/ducklake_cleanup_files.cpp:56`);
  - `run_path` defaults to `<DATA_PATH's parent>/mssql_backup_runs/<lake id>/`, so two lakes beside
    each other never share it. DuckLake keeps no catalog id: `ducklake_metadata` holds `version`,
    `created_by`, `data_path` and `encrypted` (`DL/storage/ducklake_metadata_manager.cpp:238`), and a
    `DuckLakeCatalog`'s `instance_id` is a random UUID per ATTACH
    (`DL/storage/ducklake_catalog.cpp:321`). So the lake id is a stable hash of DATA_PATH (16 hex
    digits of its SHA-256), and `plan.json` records it. The `run_path` parameter or the config key
    `import.run_path` overrides the default. When DATA_PATH is a bucket root its parent is undefined,
    so `run_path` must then be set explicitly.
- **The data files** are written at their final paths in DATA_PATH, because registration is zero-copy,
  and stay unregistered until the commit.
- **Resume** (`resume := true`): when a run of the same file exists whose plan the validation still
  accepts (no import since its S0, and no write to a planned table but a compaction, D2.3), the export
  skips the units whose files exist with their recorded sizes, and writes the rest.
  - Otherwise (the lake moved), a new run starts and the old one's files are orphans.
  - The validation refuses with "the lake moved since planning (S0 → S1, import X); run again" when it
    sees the same thing.
- **A commit that succeeded unseen**: the BackupSetGUID is in `imports`, so `if_applied := 'skip'` returns
  that snapshot.
- **Orphans**: files of runs that never registered. `ducklake_mssql_backup_cleanup_runs(lake, older_than
  := INTERVAL 7 DAY, dry_run := true)` deletes the run directories and the unregistered files under the
  `mssql-<guid>-<run>-` prefix, and never a file DuckLake lists. Before it deletes anything of a run, it
  checks the lake id that run's `plan.json` records, and leaves a run of another lake alone (an explicit
  `run_path` two lakes share).
  - **Hazard**: `ducklake_delete_orphaned_files`, a user's own call or `CHECKPOINT`'s, also deletes the
    exported data files of a run not yet committed once they are older than `delete_older_than`. An
    import that runs longer than that can lose its early files.
  - So the importer warns when an export may outlive `delete_older_than` (the plan compares its
    estimate with the option, the apply the oldest file's age), and the apply refuses to register when a
    file the manifest names is gone (its existence check, which fails closed). The docs say so.
- **After the commit**, a `ClientContextState::TransactionCommit` hook (called only after a successful
  commit, `DD/src/transaction/transaction_context.cpp:81-83`) drops the temp manifest table and the run's
  scratch files. 024's automatic policy uses the same hook.

### D10. Progress and reporting

- **The export statement** implements `table_scan_progress`: pages written / pages planned. DuckDB's
  progress bar therefore covers the export, the long phase; the transaction after it lasts as long as
  its DDL and registration, plus 023's `DELETE` scans and a SWITCH's copy (D1).
- **DuckDB's logger** (type `mssql_ducklake`): the plan summary; per table and unit start and finish,
  rows, files, bytes and seconds; the commit's duration and statement count.
- **The run directory's per-unit manifests** give progress to another session, or to the coordinator of
  external workers (`SELECT count(*) FROM read_parquet('<run>/manifest-*.parquet')`).
- `ducklake_mssql_backup_plan` shows units and bytes before anything starts. The PRAGMA returns the apply's
  summary.

### D11. A full taken under load

A full backup is fuzzy (RESEARCH §4, E3). The owner's rule is **no dirt in the lake**.

- **Before 090 (phase A)**: the reader reports a database-level `consistent` flag (no open transaction and
  no committed row operation after CheckpointLSN; exact at table level in E3), and the import passes the
  reader `fuzzy := 'error'`.
  - An inconsistent full is refused, and the refusal lists its dirty and stale tables. There is no option
    to import it anyway: RESEARCH §10's question 3 ("warn or refuse") is settled for the lake by the
    owner's no-dirt decision (R§4). For browsing, the reader defaults to `recovery := true` and
    `fuzzy := 'warn'` (DQ-13, accepted); the import passes its own arguments.
  - The counterfactual that imports it anyway (Testing, 6) is a test-only switch,
    `MSSQL_DUCKLAKE_TEST_ALLOW_DIRTY=1`, never a user option.
- **With 090**: the export replays every tail transaction and undoes the open ones (E6: exact on three
  loaded fulls, LOBs included), with `recovery := true`. The **pending set** comes from
  `mssql_backup_pending_pages` (090), from the same cached recovery: the pre-undo images of every page undo
  changed, and of every page an open transaction logged a row operation on, with the transaction ids
  (E6 §6.2). The wrapper writes it as a Parquet file and the apply registers it as
  `__mssql_sys.pending_pages`; 023 resolves it at the next file. A table recovery cannot cover is
  `unrecoverable`, and the file is refused naming it (090 D7).
- **The allocation images of a loaded full** come from `mssql_backup_allocation` with the same recovery
  parameters as the export, so `alloc_pages` keeps the images of the recovered state, which put every
  page the lake holds rows of in the IAM image of its table's AU (090).
- **The watermark for the next file** is W = min(FirstLSN, CheckpointLSN), with LastLSN recorded beside it
  (E6 rule 7 needs it).

### D12. Resources: no OOM, every core, DuckDB's limits

The owner's rule (2026-11-06): do not load everything into memory, use the machine to the full for
parallelism, never fail with OOM, respect DuckDB's memory and thread limits, and offer limits on the
import itself. mssql 085 D17 states it for the reader; this is the importer's side, and 023 and 024
cite it.

- **DuckDB's limits are the ceiling.** `memory_limit` (an alias of DuckDB's `max_memory` setting,
  `DD/src/main/config.cpp:275`) and `threads` are GLOBAL settings, for the whole instance
  (`DD/src/common/settings.json:1201-1210,1534-1543`). Spilling goes to `temp_directory`, up to
  `max_temp_directory_size`, global too (`:1517-1523,1221-1227`). The import's `max_memory` parameter
  is per call, unlike DuckDB's `max_memory` setting.
- **The import's own limits.** A per-import limit cannot be a `SET` without changing every other
  session, so the import's components enforce it: `max_memory := <bytes or '2GB'>` and
  `max_threads := <n>` on the PRAGMA (D1), `ducklake_mssql_backup_export` and
  `ducklake_mssql_backup_commit`, named as on the reader's `mssql_backup_scan` and `mssql_backup_export`
  (085 D17, 089). The defaults are the config keys `import.max_memory` / `import.max_threads`, else
  DuckDB's `memory_limit` and `threads`; a value above DuckDB's is clamped to it, with a WARNING.
- **What they bound**: the reader's allocations and units (passed to 089's `mssql_backup_export`,
  whose open files F come from `max_memory` / 4, D4, and to every reader call of the import that
  decodes or recovers: `mssql_backup_scan`, and with 090 `changed_pages`, `allocation` and
  `pending_pages`, 090 D9), and the importer's own work beside them (the per-unit manifests, the
  apply's manifest and footer checks).
- **The split of `max_memory`** (085 D17): `max_memory` is the import's whole budget. Its side
  connections get `operator_memory_limit = max_memory / 2`, for DuckDB's sorts and hash tables. The
  reader's and the importer's own reservations (decode staging, LOB values, writers, recovery
  structures) stay under the other half, and the writers get half of that: F is computed from
  `max_memory` / 4 (D4).
- **One rule for a failed reservation** (085 D17). DuckDB's `ReserveMemory` throws
  `OutOfMemoryException` through `EvictBlocksOrThrow`
  (`DD/src/storage/standard_buffer_manager.cpp:775-783`). Every reservation the reader or the importer
  makes that does not fit first waits for other units to release memory; when its unit is alone and
  it still cannot fit, it is refused by name, naming `max_memory` and `temp_directory`. With no
  `temp_directory` nothing spills (`InMemoryWarning`, `:765-773`), so the limits are hard; an
  in-memory DuckDB has one, `.tmp`, unless `temp_directory` is set to `''`
  (`DD/src/main/config.cpp:651-659`).
- **What they cannot bound** are the DuckDB statements the import runs: 023's `DELETE` scans, a
  SWITCH's copy, a sort (089's `order_by`, DuckLake's insert under a sort order), the apply's
  one-column scans (D2.3) and 023 D5's lake-side export. They run in DuckDB's executor and obey the
  instance's limits: buffer-managed, they spill. DuckDB has one per-connection memory knob at the pin,
  `operator_memory_limit` (scope local, `DD/src/common/settings.json:1260-1266`), which caps a
  connection's sorts and hash tables at the smaller of it and `memory_limit`
  (`DD/src/main/client_data.cpp:126-133`). The export's and the commit's side connections set it to
  `max_memory` / 2 (the split above), so a delete's `IN (SELECT …)` build side and a sort stay under
  it; it caps neither a scan's buffers nor DuckLake's transaction state. There is no per-connection
  thread knob (`threads`, `external_threads`, `async_threads` and `pin_threads` are global, 085 D17),
  so the statements run on `threads`: `ducklake_mssql_backup_commit` runs on a side connection, but it
  could `SET` nothing narrower, since the settings are global. So its `max_threads` is accepted for
  symmetry with the export and has no effect there. Inside an explicit transaction the statements run
  on the caller's connection, whose settings the importer leaves alone.
- **Backpressure** is the reader's (085 D17): a unit waits, or a writer closes a file. The importer
  adds no stage that holds a table's rows.
- **Out of temp space**, an export or a commit fails with DuckDB's error, which the importer reports
  naming the file and `max_temp_directory_size`; the run stays resumable (D9).

| component | home or bound |
| --- | --- |
| the export's reader: decode staging, LOB values, recovery state (090), writers | the reservations' half of `max_memory`, the writers a quarter of it, and `max_threads` (085 D17, 089) |
| the importer's statements: `DELETE` scans, the SWITCH copy, a sort, the apply's checks | DuckDB's executor: buffer-managed and spilling, under `memory_limit` and the side connections' `operator_memory_limit`, `max_memory` / 2 |
| `plan.json`, the manifests, `base/`, `delete_pages.parquet`, the moved-page lists | files on disk under `run_path` (D9), read with `read_parquet`, never held whole in memory |
| the registration's `DuckLakeDataFile` list with its column statistics | DuckLake's own transaction state (`AppendFiles` moves the files into its local changes, `DL/storage/ducklake_transaction.cpp:1842-1847`), in memory until the commit: O(files × columns), about 175 k file rows in scenario A (D7), tens of MB (estimate) |

## Enforcement & security

- **Fail closed.** These failures name the file, the GUIDs and the lake snapshots:
  - a header check or a plan-id mismatch;
  - a manifest file that is missing or the wrong size;
  - a unit counted twice;
  - a partition value that disagrees with the file's min/max (where the spec is monotone) or with a
    row's own value (`bucket`, a calendar part alone: the export's per-row check, and the apply's
    one-column scan of every manifest file, D2.3);
  - a different source database;
  - a conflict at commit that N = `import.conflict_retries` retries (N+1 attempts) did not get past
    (D1);
  - the attribution check's named refusal (089) of a page the lake gets rows of that is not on its
    header AU's IAM chain, or is on more than one, in the allocation images: `mssql_backup_allocation`
    runs it on both paths (phase A's rows from `mssql_backup_scan`, phase B's from the export), and the
    export repeats it on the pages it writes. The import refuses the file, naming it, so the next
    file's deletes can always attribute the page (023 D4).
- **The backup is untrusted input.** The reader bounds every length (mssql §9). The importer also checks
  every manifest against the file it names: the row count and the statistics must agree with the
  Parquet footer.
- **Keys.** The import has no key parameter. The reader selects a `mssql_backup_key` secret by scope, then
  by thumbprint (087). Secrets are per instance, so the side connections see them; verify this for
  temporary secrets. Provenance records the thumbprint only, and the generated SQL and the logs carry no
  secret.
- **What the lake exposes.** The lake holds the database's data. `__mssql_sys` holds catalog rows; module
  source (procedures, functions, triggers) is not mirrored, only view definitions, partition boundaries,
  and computed-column and default definitions (DQ-11). `pending_pages` holds uncommitted rows until 023
  resolves them (DQ-09). Type-101 pad bodies never reach the lake (the reader skips them, E2 R10).
- **TDE backups** are refused until E10 reads their log tail: the reader reports `fuzzy_status =
  'unknown'` for every table of a TDE backup (087 D9), and no dirt may enter the lake.
- **Encrypted lakes** (DuckLake `encrypted`) are refused in v1. The export cannot encrypt with DuckLake's
  per-file keys, and `ducklake_add_data_files` refuses them too
  (`DL/functions/ducklake_add_data_files.cpp:45-48`).
- **Version mismatches.**
  - A ducklake bump re-audits the apply (it mirrors `AddWrittenFiles`), next to spec 014's closed list,
    and re-runs the metadata-row comparison with `ducklake_add_data_files` (Testing).
  - The manifest carries the reader's version; a run whose workers report different mssql versions is
    refused.
- **Concurrent importers** on one lake: the plan id and DuckLake's conflict check (two transactions
  creating the same schema or table conflict, `DL/storage/ducklake_transaction_state.cpp:191-205,265`)
  make the loser fail cleanly; its retry's validation then sees the other import and refuses (D1, D2.3).
  The run id keeps their files apart.

## Testing

**Lanes.**

1. **Replay, no server**: a versioned corpus of backups plus truth dumps (subsets of E1, E2, E5 and E6,
   fetched in CI), imported into DuckDB-file lakes, with assertions against the truth files. Fast and
   deterministic.
2. **Live** (`test/sql/integration/backup/`, `MSSQL_DUCKLAKE_TEST_DSN`):
   - the docker server takes the backups onto a backup volume shared with the host (a compose change);
   - the oracle RESTOREs the same files;
   - the lake's catalog is in SQL Server, so the commit batch is exercised.

**The oracle.** For each table, the two sides must be multiset-equal over canonical rows, compared both ways
with `EXCEPT ALL`:

- the sides: `lake.s.t AT (VERSION => n)` and `mssql_scan` of the restored database;
- the canonical form: LOBs as SHA-256 plus length, floats as IEEE hex, strings and binaries as bytes;
- locators: heap `_mssql_rid` = `0x00 ‖ %%physloc%%` (RESTORE keeps physical pages), clustered
  `_mssql_page` = the physloc page in the `01` form;
- also: row counts, and the lake schema against the expected type map;
- one macro, `mssql_backup_oracle(lake, version, restored_db)`, returns the differing keys per table.

**Lake against lake**: phase A and phase B over the same file give equal rows, mirrors and allocation
images, up to file names and file boundaries.

**Phase B against `ducklake_add_data_files`**: the same files, registered once by the apply and once by
`ducklake_add_data_files`, give the same metadata rows (`ducklake_data_file`,
`ducklake_file_column_stats`, `ducklake_file_partition_value`, name maps, table stats), up to ids and
snapshot numbers (D7).

**Scenarios (full import):**

- E1's shapes: a heap with forwarded rows; a clustered table with page splits; a non-unique clustered
  index with uniquifiers; a LOB table (in-row, off-row, a 630 KB value); ROW compression; columns dropped
  and added before the backup;
- E2's shapes:
  - two data files and a second filegroup; a striped backup;
  - CHECKSUM, BLOCKSIZE and MAXTRANSFERSIZE variants;
  - a shrunk file, where a naive overlay doubled the rows;
- E5's CCI tables: delete bitmap, open delta store, ordered and partitioned CCI;
- partitioned tables: E7's structure database (`experiments/E7/sql/a00`–`a03`: 18 functions, RANGE LEFT
  and RIGHT, int / bigint / date / datetime / datetime2(0/3/7) / datetimeoffset / string parameters, an
  empty function, misaligned and non-aligned functions, rows on every boundary edge). Assert:
  - the proposed spec equals E7§3's "exact rule" column (e.g. datetime2(7) RIGHT monthly → `epoch_month`,
    datetimeoffset `+02:00` boundary → `epoch_year`, strings and int ranges → none);
  - every file's partition value equals DuckLake's own value for each of its rows, at the pin: under an
    `epoch_*` spec the `datetime2(7)` rows at b − 100 ns and b − 500 ns stay in their period's file
    (floored, D5), and the `datetimeoffset` table imported under `TimeZone = 'UTC'` and under
    `'Europe/Berlin'` gets the same values (the UTC instant); under a `year` override on it each session
    gets its own (E7§3's `duck_check_berlin.csv`, re-run at the pin, task 4); every lake equals RESTORE;
  - a `bucket(n)` override and a lone `month` override: the export checks every row's value, and a
    constructed file whose rows disagree, handed in through `manifests :=` as an external worker's, is
    refused by the apply's one-column scan (D2.3);
  - files are split by DuckLake value, not by SQL Server partition (a SQL Server partition whose rows
    share one value may share files with its neighbours under no spec);
  - min/max pruning on the partition column (partition values prune only `bucket` at this pin, E7§3);
- compressed (MS_XPRESS, ZSTD) and encrypted samples (087), and every type in 086's matrix; a TDE sample
  is refused by name until E10 (087 D9), then imported;
- edge cases: an empty table, a table with no rows but allocated pages;
- a 2,000-table, 40-column catalog, for the commit volume.

**Load (E6)**: a full taken while T1 commits later, T2 rolls back, T3 commits after touching other pages,
T5 commits during the backup, and committed LOB updates run.

- Before 090 the import refuses by default and lists the tables.
- With 090 the lake equals RESTORE (E6: 0 differences, LOBs included), and the pending set equals the pages
  the open transactions touched (E6: 3,372).

**Counterfactuals.** Each switch is test-only (`MSSQL_DUCKLAKE_TEST_*`), and each shows a rule is
necessary:

1. (removed)
2. `_mssql_page` in `skip_stats_columns`: a later delete of one page reads every file of the table (the
   file count is recorded), where with its statistics it is expected to read the few files whose min/max
   hold the page (task 11's checks). The rows are the same either way. That is why D3 never skips it.
   `_mssql_page` as a BLOB reads every file the same way (no file filter, D3), which is why it is
   VARCHAR.
3. A sort on a string key under a case-insensitive collation: after a DuckLake compaction a page's rows
   sit in many files, and a delete of one page reads them all (the file count is recorded). That is why
   D5 withholds the sort.
4. No run id in the names: two concurrent imports overwrite each other's files (a C++ test).
5. The plan-id check off: an import planned before another one committed registers stale DDL. With the
   check on, it is refused.
6. `MSSQL_DUCKLAKE_TEST_ALLOW_DIRTY=1` on a loaded full: dirty keys appear (E6: 7,516). That is why the
   import refuses, and why the switch is test-only (it replaces the drafted `fuzzy := 'allow'`).

**The commit**: scenario A against the SQL Server catalog, recording the statements, the rows and the
time, with `MSSQL_DUCKLAKE_STRICT_BATCH=1`.

**Memory**: a wide table under a `bucket(16)` override, cut into many units, exported with `SET threads =
16` and a small `memory_limit`: the writers' buffers stay under their share, a quarter of the limit
(`duckdb_memory()`), also with LOB values far wider than the estimated row width, and the export
finishes, with fewer units running at once and more, smaller files and row groups (D4). A backup much
larger than memory (mssql's generated 20 GB full, 085 acceptance 13) imported in phase B with
`memory_limit = '1GB'` and every thread completes: the peak memory DuckDB accounts (`duckdb_memory()`,
the buffer manager's used bytes) stays under the limit, and the lake equals the oracle. With
`max_memory := '512MB', max_threads := 2` likewise, at lower concurrency, the memory the export
reserves under its half, 256 MB (D12's split); a 023 differential of it commits under the same limits
(D12).

**Failure**:

- a kill between export and commit, after which resume reuses the files;
- an injected commit failure, after which the files are kept and the retry succeeds;
- an already-applied file, which is skipped and returns its snapshot id;
- `cleanup_runs`, which deletes only unregistered run files, and nothing of a run whose `plan.json`
  records another lake's id; two lakes under one parent get two default `run_path`s;
- a manifest naming a deleted file, which is refused;
- a conflict at commit (023 Testing, writes by others), retried on the same export and reported after
  N = `import.conflict_retries` retries (N+1 attempts), in phase B and in phase A (D2.4); inside an
  explicit transaction the COMMIT fails, and a second run resumes from the export;
- a `CHECKPOINT` with a short `delete_older_than` during an export: the run's scratch, outside
  DATA_PATH, survives; an exported file it removed is logged by the warning and makes the apply refuse
  by name (D9).

## Alternatives considered

- **`INSERT … SELECT FROM mssql_backup_scan` in one transaction** (research §4.3 option A): the lake
  transaction lasts the whole import, a rollback deletes every file, and big tables run one after
  another.
- **Writing DuckLake's metadata rows directly**, as `iceberg_to_ducklake` does: format 0.4 and an empty
  catalog only, `row_id_start` left NULL, no conflict checks, and hand-written T-SQL to re-validate at
  every DuckLake bump.
- **A `CALL` table function doing the DDL through the Catalog API**: binding `PARTITIONED BY` / `SORTED BY`
  / `WITH` by hand, where the SQL is the stable contract (D1).
- **`ATTACH` the backup, then `COPY FROM DATABASE`**: one UNION ALL, big tables serialised.
- **One snapshot per table**: against the owner's decision.
- **Setting the inlining option lake-wide**: changes the user's tables (D6).

## Decisions

Each is decided or accepted, in the list merged with the mssql series:
[decisions.md](https://github.com/hugr-lab/mssql-extension/blob/main/specs/085-backup-reader-core/decisions.md).

| # | question | decision | DECISIONS |
| --- | --- | --- | --- |
| 1 | the run id in file names | kept (D4) | DQ-34 |
| 2 | Parquet field ids or name maps | name maps in v1 (D2.3) | DQ-24 |
| 3 | service column types | opaque values, each led by a shape: `_mssql_page` a hex VARCHAR so file statistics prune deletes (owner, 2026-11-06), `_mssql_rid` and `_mssql_lob` BLOBs; raw LOB bytes only where a value does not re-encode (D3) | DQ-02, with DQ-01 for the LOB bytes |
| 4 | nullability | every lake column nullable (D3) | DQ-23 |
| 5 | the fuzzy default | refuse (no dirt, R§4); the user option is gone (D1, D11) | settled |
| 6 | one source database per lake in v1 | yes (D2.1) | DQ-22 |
| 7 | `bucket` overrides | allowed, with a warning naming the cost (D5) | DQ-26 |
| 8 | proposing `SORTED BY` only for keys that order like DuckDB's | yes: the clustered rowstore key, when the physical order satisfies it (D5) | DQ-25 |
| 9 | what the mirror holds | the allow-list; of `sysobjvalues` only view definitions, partition boundaries, and computed-column and default definitions, no module source (D3) | DQ-11 |
| 10 | encrypted lakes | refused in v1 (§ Enforcement) | DQ-28 |
| 11 | LOB maps in a phase-A lake | 085 PR 3 carries the plain `_mssql_lob` output, so every lake has them (D2.4) | DQ-05 |

## Follow-ups

- **Views from backups (decided here: not a spec).** The text is captured in `__mssql_backup.views` and
  versioned by 023. Translating a SELECT-only subset (brackets, TOP, ISNULL, CONVERT, `+` concatenation)
  into DuckDB views is opt-in later, either per view or for the subset that parses. DuckLake stores only
  DuckDB SQL (`DL/storage/ducklake_transaction.cpp:1264`; `ducklake_view_entry.cpp:115-119`).
- **Several source databases per lake** (`schema_prefix`, and per-database `__mssql_sys_<db>`).
- A schema of views over the imported tables without their service columns.
- The server-side apply extended to import commits (D7), if the measurement asks for it.
- A coordinator for external workers: leases on units, retries.
- Log backups (RESEARCH phase 4) as a later spec in this series.

## Tasks

| # | task | estimate |
| --- | --- | ---: |
| 1 | Registration and options: the PRAGMA, `_plan`, `_history`; option parsing; refusing encrypted lakes; test scaffolding (both lanes, the corpus fetch, the docker backup volume) | 3 d |
| 2 | Planning: the header and lake state through a side connection, the fresh-lake checks, filters, the plan id, `plan.json`, the run directory | 3 d |
| 3 | DDL generation: schemas, tables, service columns, type adjustments, `WITH` options, the mirrors, `__mssql_sys` and `__mssql_backup` tables | 3 d |
| 4 | Partition and sort proposal from 085's `partitioning` (E7's rule: inner partitions, ticks, RANGE LEFT and RIGHT, `datetimeoffset` in UTC instants), coarsening, overrides, the `OrdersLikeDuckDB` rule, recording; the partition values through DuckLake's bound `ApplyPartitionTransform` rendered to SQL, for phase A as for 089's `partition_by` (the rendering verified for every transform and type, `bucket` included); the side connection's TimeZone and Calendar for calendar transforms on TIMESTAMPTZ; E7§3 re-run at the pin (`epoch_*` on the UTC instant, `_NS` floored); E7's expectations as tests | 3.5 d |
| 5 | Phase A: `COPY` per table and partition, the mirrors and allocation images, `ducklake_add_data_files`, provenance, the commit message; in autocommit through `ducklake_mssql_backup_commit` (its side connection and the bounded conflict retry, `import.conflict_retries`, built here for phase A) | 4 d |
| 6 | The oracle harness: the RESTORE side, the canonical rows macro, the locator checks; quiet-full scenarios | 5 d |
| 7 | Phase B registration: manifest → `DuckLakeDataFile`, `AppendFiles`, name maps (or field ids), partition values and their check (min/max, or the one-column scan under `bucket` or a lone calendar part), skipped statistics, the importer's own files | 5 d |
| 8 | Phase B export: the `ducklake_mssql_backup_export` wrapper (side connection, Parquet settings, `max_memory` and `max_threads` for the reader's cap, their config keys and the clamp, D12, per-unit manifests), external-worker manifests and their validation; the memory test and the 20 GB import | 5 d |
| 9 | Failure handling: resume (across compactions), `ducklake_mssql_backup_validate` (plan id, foreign writes, compactions let through), the bounded conflict retry extended to phase B's apply (task 5's `ducklake_mssql_backup_commit`; its `max_memory`, the side connections' `operator_memory_limit` = `max_memory` / 2, D12), `if_applied`, `cleanup_runs` (the lake id check), `run_path` (the lake id in its default) and the `delete_older_than` warning, the post-commit hook | 3.5 d |
| 10 | Progress (`table_scan_progress`), logging, the summary rows | 1 d |
| 11 | Delete cost and standard compaction, two checks (with 023 task 6): the open one, that a `DELETE … WHERE _mssql_page IN (SELECT …)` prunes the VARCHAR column's files through DuckDB's dynamic join filter, as mssql-ducklake measured for BIGINT (if not, 023's expansion passes the delete set as constants, 023 D4), after N differentials and after a merge; and a test that keeps the answered one: on a SQL Server catalog the file filter orders the fixed-width uppercase hex as bytes, under the statistics columns' `Latin1_General_100_BIN2_UTF8` (D3); a `CHECKPOINT` on an imported lake, counterfactuals 2 and 3 | 1 d |
| 12 | The commit: the 2,000-table measurement on the SQL Server catalog, strict-batch shapes added to spec 014's list, the lake-vs-lake check of phase A against B, the metadata rows of phase B against `ducklake_add_data_files` | 3.5 d |
| 13 | A loaded full: `fuzzy` before 090; after 090 the pending set and the allocation images of the recovered state (`mssql_backup_allocation` with the recovery parameters) registered | 2 d |
| 14 | The docs page (`website/docs/`), the README index row | 2 d |
| | **total** | **44.5 d ≈ 9 wk** |

Phase A (tasks 1–6, 10, 14) is 24.5 d, **about 5 weeks**, needing 085 and 086 (23.5 d before the
bounded retry's core moved from task 9 to task 5, so that phase A's registration gets it). Phase B
(tasks 7–9, 11–13) is 20 d, **about 4 weeks**, needing 089 (21 d before that move, 20 d before D12's
limits joined tasks 8 and 9). That is series.md's 6.5–9 weeks, depending on how far the harness work
(task 6) overlaps the rest.


> **Owner, 2026-11-05 and 2026-11-06 (DQ-02):** every `_mssql_*` service column is an opaque value
> serialized and read only by the C++ code, each starting with a shape. `_mssql_page` is hex VARCHAR so
> that DuckLake's file statistics prune deletes (owner, 2026-11-06): a page `01` + 4 hex digits of file
> id + 8 of page id, a compressed columnstore rowgroup `02` + 16 hex digits of rowset id + 8 of rowgroup.
> The others are BLOB. No `_mssql_rowgroup`.
