# Spec 022: importing a full SQL Server backup, one DuckLake snapshot per file

- **Status**: draft (number tentative)
- **Date**: 2026-11-05
- **Author**: vgsml, with Claude

## Summary

`PRAGMA ducklake_import_mssql_backup('lake', 'sales_full.bak')` turns a full backup file into one DuckLake
snapshot, with no SQL Server instance anywhere. The work is done in three phases:

- **Plan.** Read the backup's header and catalog through mssql's file functions (085), and read the lake's
  state at its latest snapshot.
- **Export.** Parallel workers write every data file, plus a record of which rows each SQL Server page
  occupies. They run outside any lake transaction.
- **Register.** One short transaction runs the DDL and registers every file through DuckLake's own
  transaction (`AppendFiles`). It also creates the `__mssql_sys` mirror of the SQL Server base tables, the
  page map, and the provenance rows, and sets the commit message.

The lake this spec creates is the base that 023 applies differentials and later fulls to, and its
snapshots are the backup history that 024 keeps and expires. This spec also fixes the decisions the lake
is born with: the service columns, the partitioning and sort order proposed from SQL Server once, at
creation (after that the layout is DuckLake's), inlining off, and importer-owned compaction.

Owner questions are cited as DQ-nn ([decisions.md](https://github.com/hugr-lab/mssql-extension/blob/main/specs/085-backup-reader-core/decisions.md), merged across both series). **Owner decision
(2026-11-05): follow DuckLake's rules, do not replicate SQL Server** (DQ-10). SQL Server's partitioning
only proposes the initial DuckLake spec; partition values are computed exactly as DuckLake computes
them, in the importing session; files are laid out by DuckLake partition values, sort order and file
size, never per SQL Server partition (those are work units only); a SWITCH is a lake-side copy plus
positional deletes (023 D8).

## Problem

There is no way to get a SQL Server backup into a lake without a server: today it takes RESTORE, then
`mssql_scan` or COPY, table by table, with one snapshot per statement. The owner's operating model needs
more than a copy (RESEARCH §6.2):

- one snapshot per backup file, so that time travel *is* the backup history;
- a lake that can take the next differential with nothing but itself and that file. That needs row
  locators (`_mssql_rid`, `_mssql_page`), the page → file positions, the SQL Server catalog, and the LOB
  fragment maps, all stored in the lake from the first import (E1 rules 5–8, E2 R5);
- an import that scales: 1 TB and thousands of tables, parallel across tables and within them, and a
  commit that does not hold a lake transaction open for hours.

DuckLake's own write paths do not fit:

- An `INSERT … SELECT FROM mssql_backup_scan` runs the whole import inside one lake transaction, and a
  rollback deletes every file written so far (`DL/storage/ducklake_transaction.cpp:839-853`).
- `COPY FROM DATABASE` puts the tables in one UNION ALL, whose saturating children run one after another
  (research-ducklake-import §3).
- Rotation turns off order preservation, so the producer never knows where a page's rows went
  (`DD/src/execution/physical_plan/plan_copy_to_file.cpp:28-36`).

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
| `threads` | DuckDB's | export parallelism |
| `mode` | `'auto'` | `'fresh'` requires a lake without this database; 023 adds `'incremental'` and `'rebase'` |
| `if_applied` | `'skip'` | a file whose BackupSetGUID is already in `__mssql_backup.imports` is a no-op that returns its snapshot; `'error'` refuses it |
| `manifests` | NULL | register files written by external workers (D4) instead of exporting |
| `resume` | true | reuse the files of an unfinished run of the same file planned at the same lake state (D9) |
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
registration re-validates (D2.3).

**The expansion** (phase B; phase A is in D2.4):

```sql
-- the callback (D2.1) plans at the lake's latest snapshot S0 through a side connection and writes
-- <data_path>/__mssql_backup/runs/<BackupSetGUID>/<run>/plan.json; then:
CREATE TEMP TABLE __mssql_backup_manifest_<run> AS
    SELECT * FROM ducklake_mssql_backup_export('lake', 's3://bak/sales_full.bak', plan := '<run dir>/plan.json');
CREATE SCHEMA lake.__mssql_sys;  CREATE SCHEMA lake.__mssql_backup;  CREATE SCHEMA lake.dbo;
CREATE TABLE lake.dbo.orders (id BIGINT, order_date TIMESTAMP, …, _mssql_rid BLOB, _mssql_page BLOB)
    PARTITIONED BY (epoch_month(order_date)) SORTED BY (id)
    WITH (data_inlining_row_limit = 0, auto_compact = false, skip_stats_columns = ['_mssql_rid']);
-- … every table, every mirror, page_map, alloc_pages, pending_pages, stale_pages, the registry tables
SELECT * FROM ducklake_mssql_backup_apply('lake', 's3://bak/sales_full.bak',
    manifest := '__mssql_backup_manifest_<run>', plan := '<run dir>/plan.json');
```

The order matters.

- **The export comes first.** The statement touches no lake catalog entry; it uses only the temp catalog
  and side connections. So DuckLake's transaction, and with it the metadata connection's server
  transaction (`DL/storage/ducklake_transaction.cpp:856-897`), is not open during the hours of writing.
- **The first `CREATE` opens the lake transaction.**
- **The apply's summary is the last result**, so it is what the PRAGMA returns. The apply also sets the
  commit message itself, because it alone knows the counts the message carries.

**Companion functions** (table functions; usable by hand):

| function | does |
| --- | --- |
| `ducklake_mssql_backup_plan(lake, file, …)` | the callback's work as rows: chain decision, DDL, specs and why, units, bytes, the expansion text. Read-only |
| `ducklake_mssql_backup_export(lake, file, plan :=)` | the export phase (D2.2); returns the manifest |
| `ducklake_mssql_backup_apply(lake, file, manifest :=, plan :=)` | the registration (D2.3); returns the summary |
| `ducklake_mssql_backup_history(lake)` | imports with snapshot ids, generation, consistency, expired (024 extends it) |
| `ducklake_mssql_backup_compact(lake [, schema, table], …)` | the importer's compaction (D6) |
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
  lake's options.

The side connection first takes the importing session's `TimeZone` (and `Calendar`), so a
`TIMESTAMPTZ` partition value is the one a DuckLake writer in that session would compute (DQ-10); the
plan records it for external workers (D4). The data files and their page runs come from 089's export.
The other kinds are written by the wrapper itself with `COPY … TO … (FORMAT parquet)` from the reader's
functions, their page runs computed from `file_row_number` as in phase A: the mirrors from
`mssql_backup_catalog(file, t)`, the allocation images from `mssql_backup_allocation`, and (with 090)
the pending set from `mssql_backup_pending_pages`. It returns one manifest row per file written:

- kind: data, page_map, mirror, alloc, or pending;
- target lake table, path, row_count, file_size_bytes, footer_size, row_group_count;
- the column statistics, in the `WRITTEN_FILE_STATISTICS` shape the Parquet writer already produces;
- partition values, unit id, BackupSetGUID, reader version.

Per-unit manifests are also written into the run directory as units finish. That makes the run resumable,
and observable from other sessions (D10). The wrapper never touches the caller's lake transaction.

**D2.3 Register (one short transaction).** `ducklake_mssql_backup_apply` runs in
`DuckLakeTransaction::Get(context, lake)`, after the DDL statements:

1. **Re-validate** at the transaction's snapshot S1:
   - the plan id still holds: no import committed since S0, and this BackupSetGUID is not in `imports`
     (else D9);
   - no non-import snapshot since S0 wrote to a table the plan touches (DQ-08);
   - every manifest file exists with its recorded size;
   - every planned unit appears exactly once.

   Anything else fails, naming the file, the GUIDs and the snapshots.
2. **Data files.** Each manifest file becomes a `DuckLakeDataFile`
   (`DL/include/common/ducklake_data_file.hpp:65-83`), built the way `DuckLakeInsert::AddWrittenFiles`
   builds one (`DL/storage/ducklake_insert.cpp:157-258`):
   - statistics keyed by `FieldIndex` (resolved through `table.GetFieldId`), with the bounds of
     `skip_stats_columns` cleared;
   - `partition_id` taken from the table's spec, and `partition_values` as reported. Each value is checked
     by evaluating DuckLake's own expression, in this session, on the file's min and max of the partition
     column (the transforms are monotone), and a file that spans two values is refused;
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
expanded into DuckDB SQL only:

```sql
COPY (SELECT *, _mssql_rid, _mssql_page,   -- virtual columns are not in *; add _mssql_lob for a
                                            -- table with off-row-capable columns (089 PR 1, DQ-05)
               date_diff('month', DATE '1970-01-01', order_date) AS epoch_month  -- DuckLake's expression
        FROM mssql_backup_scan('…', 'dbo', 'orders'))
  TO '<data_path>/dbo/orders/'
  (FORMAT parquet, PARTITION_BY (epoch_month), FILE_SIZE_BYTES '<the lake''s target_file_size>',
   FILENAME_PATTERN 'mssql-<guid>-<run>-o1029578706-{i}');
-- … per table, and per mirror (mssql_backup_catalog(file, 'sysschobjs')) …
CREATE SCHEMA …;  CREATE TABLE … WITH (…);
CALL ducklake_add_data_files('lake', 'orders', '<data_path>/dbo/orders/**/mssql-<guid>-<run>-*.parquet',
                             schema := 'dbo', hive_partitioning := true);
INSERT INTO lake.__mssql_sys.page_map                       -- runs, from the files as written
  SELECT … FROM (SELECT filename, file_row_number, _mssql_page
                 FROM read_parquet([...], filename := true, file_row_number := true)) …;
INSERT INTO lake.__mssql_backup.imports VALUES (…);  -- … tables, columns, views, config
CALL ducklake_set_commit_message('lake', 'mssql-ducklake', 'full sales_full.bak', extra_info := '{…}');
```

- `ducklake_add_data_files` maps columns by name, reads statistics from the footers, appends to the
  caller's transaction, and is not rolled back on failure (research-ducklake-import §2).
- The page map is computed from the written files, so correctness never depends on where rotation put a
  page's rows. The cost is a map with more runs. Its `au_id` per file comes from the plan (a `COPY`
  covers one table, and every row of a rowset is on its one IN_ROW AU); its `lsn` stays NULL (D3).
- Partition values come only from Hive paths (`DL/functions/ducklake_add_data_files.cpp:1084-1120`). The
  `COPY` computes them per row with the SQL text of DuckLake's own expression for the spec (D5), in the
  importing session, so any identity / calendar / `epoch_*` spec works in phase A, overrides included; a
  `bucket` spec waits for phase B unless its DuckLake expression is callable from SQL (to verify).
- `_mssql_lob` is 089's column (PR 1). A phase-A lake written before it lands has no LOB maps, and 023
  then refuses partial LOB updates on it; whether to move the plain map output into 085 is DQ-05.
- Phase A runs the tables one `COPY` after another, each `COPY` parallel inside, in the order DuckDB's
  COPY gives; correctness never depends on it, and the files follow DuckLake's partition values and
  file size like DuckLake's own INSERT.
- Phase B must produce the same lake as phase A, up to file names and run boundaries. A lake-vs-lake
  comparison keeps the two honest (Testing).

### D3. The lake a full import creates

**User tables**: the table's columns, then the service columns.

| service column | type | content |
| --- | --- | --- |
| `_mssql_rid` | BLOB | the reader's row locator (RESEARCH §5): a heap's original file:page:slot (following the forwarded back pointer); a clustered table's key + uniquifier; a CCI's (rowgroup, tuple). Opaque, and encoded by 085 |
| `_mssql_page` | BLOB | the page the row lives on now (`u16 file ‖ u32 page`, big-endian); NULL for a compressed columnstore row |
| `_mssql_lob` | BLOB | only on tables with off-row-capable columns (LOB types, row-overflow). Opaque bytes serialized by the C++ code (DQ-02, decided): per off-row value its SQL Server column id (a rename changes nothing), the root bytes, the fragment map (E1 rule 7, E2 R8) and, per DQ-01's hybrid, the raw old bytes only where the lake value does not re-encode exactly (an unpaired surrogate in UTF-16, an unmapped code-page byte) and for `xml` / `json` / CLR values |

Type adjustments for the lake, on top of 086's map:

- strings are plain `VARCHAR`: DuckLake refuses alias and collated types (`DL/common/ducklake_types.cpp:158-184`).
  The declared length, collation and code page are kept in `__mssql_backup.columns`;
- `vector` → `LIST(FLOAT)` (DuckLake lists no fixed-size ARRAY; to verify);
- `sql_variant` → `VARIANT`; geometry and geography → `GEOMETRY` (WKB from 086); `json` → `JSON`;
  `xml` → `VARCHAR`;
- identity columns become plain columns (seed and increment in the registry);
- persisted computed columns become plain columns. Non-persisted ones are not in the lake: DuckLake
  refuses generated columns, so their text goes into the registry;
- every lake column is nullable (DQ-23).

**Table options, at CREATE**: `WITH (data_inlining_row_limit = 0, auto_compact = false, skip_stats_columns
= [...])`. The skipped statistics are `_mssql_rid`, `_mssql_lob` and every `(max)` column. Their
bounds prune nothing, and a min or max past the catalog's `stats_length` turns the appender off for the
whole commit (specs/018, `MD/mssql_metadata_manager.cpp:26-31`). Why per table and not lake-wide: D6.

**`__mssql_sys`** (the physical state the next file needs; all of it created by the first import):

| table | content |
| --- | --- |
| one mirror per base table `mssql_backup_catalog(file)` lists (085's allow-list) | the raw rows of `sysschobjs`, `syscolpars`, `sysrowsets`, `sysallocunits`, `sysrscols`, `sysidxstats`, `sysiscols`, `sysseobjvalues`, `sysclsobjs`, the columnstore tables (`syscsrowgroups`, `syscscolsegments`, `syscsdictionaries`), E7's partitioning tables (`systypedsubobjs`; `syssingleobjrefs` classes 7, 8, 30, 31), and `sysobjvalues` (rows for view text and partition boundaries, valclass 30, only: not module source; DQ-11). Each has its own `_mssql_rid`, `_mssql_page` (E1 rule 6) |
| `page_map` | `(page_range BIGINT, page BIGINT, kind UTINYINT, au_id BIGINT, file_path VARCHAR, first_pos BIGINT, first_tuple INTEGER, row_count INTEGER, lsn DECIMAL(25,0))`, `PARTITIONED BY (page_range) SORTED BY (page)`, with `page_range = page >> 20`. One row per run; positions are 0-based row numbers in the file. `kind` 0 = rowstore page (`first_tuple` NULL); 1 = a run of surviving tuples of a columnstore rowgroup (`page` = rowgroup id, `au_id` = the columnstore rowset, a bitmap entry for tuple t maps to `first_pos + (t − first_tuple)`, 088 D3). **The page map records the AU (columnstore: the rowset), not the table**, because a SWITCH changes the table and leaves the AU (E7§4); the table is resolved through the mirror at each import. One global table, because "delete by page in every table" (E1 rule 5) is then a single lookup. `file_path` as `ducklake_list_files` reports it, because file ids exist only after the commit. `lsn` = the page's LSN from 089's page runs (R§6.2 point 4, E2 R5; NULL in phase A); no rule reads it, it is diagnostic |
| `alloc_pages` | `(page, kind, lsn DECIMAL(25,0), image BLOB)`: the latest image of every allocation page (file header, GAM, SGAM, PFS, DCM, BCM, IAM; E2 R5), a side input to 089 |
| `pending_pages` | `(file_id, page_id, page, xids UBIGINT[], lsn, image BLOB, import_seq)`: `mssql_backup_pending_pages`'s rows (090) plus the import that produced them; filled by a loaded full (D11); passed back as `base/pending.parquet` and resolved by 023. They hold uncommitted rows: DQ-09 |
| `stale_pages` | `(page, reason, import_seq, skipped_xids UBIGINT[])`, `reason` as 090 reports it (`baseless`, `skipped_transaction`, `lob`): created empty; 023 fills it |

LSNs are `DECIMAL(25,0)` in SQL Server's numeric form (vlf · 10¹⁵ + block · 10⁵ + slot), which orders the
way LSNs order and is the form HEADERONLY prints.

**`__mssql_backup`** (the importer's own state):

| table | content |
| --- | --- |
| `imports` | one row per applied file (D8) |
| `tables` | one row per imported object: object_id, the SQL Server schema and name, shape (heap / clustered / CCI), the lake schema and table it lives in, the import that created it, `created_partitioning`, `created_sorting`, `spec_source` (override / derived / none) and why, status (live / dropped) |
| `columns` | object_id, colid → the lake column, SQL Server type text, declared length, collation, nullability, default text, computed text, identity seed and increment |
| `views` | object_id, schema, name, the T-SQL definition (from `mssql_backup_views`) |
| `config` | key/value: the filters, retention policy (024), compaction policy, and the loaded-diff mode (023) |

**Sizes** (estimates, except where measured):

| piece | size | source |
| --- | --- | --- |
| `page_map` | about one run per used 8 KB page: ~131 M rows per TB, roughly 0.5–1 GB of Parquet (not measured) | research-ducklake-import §5 |
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
- heap: IAM extent ranges in ascending page order, so `_mssql_page` min/max prunes too;
- clustered columnstore: rowgroups in tuple order, with delta stores as pages.

Every unit runs at once on DuckDB's scheduler. A big table does not wait behind another, as it would in
`COPY FROM DATABASE`.

**The writer contract** (research-ducklake-import §5, 089 § Surface), which 089 honours:

- one thread per open file; in physical order a page's rows are emitted together;
- a page is split only at a rotation or at a change of DuckLake partition value. When the table's
  DuckLake sort order is not satisfied by the physical order, the unit is sorted first and a page's rows
  may scatter over many runs (correct, but a bigger page map; DQ-25);
- each file reports its runs `(page, page_lsn, first_pos, row_count)`, positions being 0-based Parquet row
  numbers. A columnstore run is `(rowgroup, first_tuple, first_pos, count)` over the surviving tuples, so
  tuple *t* of rowgroup *g* sits at `first_pos + (t − first_tuple)` of the run that holds it (088 D3);
- one open file per partition value per unit;
- memory is one row group (122,880 rows) per open file, plus the sort's spill when a sort is needed.

**File names**: `<table data path>/mssql-<BackupSetGUID>-<run8>-o<object_id>-p<partition>-u<unit>-<seq>.parquet`
(passed to 089 as `naming :=`), plus a page-map file per unit under `__mssql_sys/page_map/`. They are
deterministic within a run, which is what resume relies on. The run id keeps two concurrent attempts on
one lake from writing the same object. The owner asked for names keyed by BackupSetGUID / table / chunk,
which these are; DQ-34 asks whether the run id may stay. `p<partition>` names the work unit's SQL Server
partition only; it says nothing about the file's DuckLake partition value.

**External workers.** `ducklake_mssql_backup_plan` writes the plan. Other processes or machines run
`mssql_backup_export(file, out := …, units := [...], …)` with the arguments `plan.json` lists
(`since_lsn`, `base`, `partition_by`, `order_by`, `target_file_size`, `parquet_options`, `naming`), in a
session set to the plan's `TimeZone` (D2.2), with write access to the data path only (they never touch the
lake catalog), and each drops `manifest-<units>.parquet` (the export's result rows) into the run
directory. Then `PRAGMA ducklake_import_mssql_backup(…, manifests := '<run dir>/manifest-*.parquet')`
validates and registers. A side connection writes files only, never lake rows (research-ducklake-import
§8).

**Side inputs and outputs.** A fresh full needs no side input. For a loaded full, the wrapper writes the
pending set from `mssql_backup_pending_pages` (D11). 023 adds the side inputs under the run's `base/`
(catalog, allocation, LOB, pending; 089 D10).

### D5. Partitioning and sort: proposed once, at creation; DuckLake's afterwards

**Owner decision (2026-11-05, DQ-10): follow DuckLake's rules, do not replicate SQL Server.** Once the data
has left SQL Server, its physical placement is DuckLake's business. SQL Server's partitioning and
clustered key only *propose* the spec a lake table is created with; the partition values, the file layout,
the sort order and the file size are DuckLake's, exactly as for any DuckLake writer.

Per table, when the lake table is created (at this import, or when a table first appears in a later file,
023). The importer never changes the spec afterwards; manual DuckLake changes win, and later imports write
under whatever spec the table has then.

1. **An override from the call wins**: `partitioning := MAP {'s.t': '<spec>' | 'none'}`, and likewise
   `sorting`. The plan refuses an override whose column does not exist or whose transform does not apply
   to the column's lake type. A `bucket(n, col)` override is refused in v1, because it scatters a page's
   rows over n files and makes one run per row (DQ-26). A bucket spec a user sets later with DuckLake's
   own ALTER is written like any other, since manual changes win.
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
     `t_dto_r` → `epoch_year`). It is only a proposal; the values are step 3's;
   - if the average partition holds less than a quarter of `target_file_size` (estimated from
     `rcrows`), T is coarsened, so tiny daily partitions do not become tiny files;
   - `epoch_hour` is not proposed (E7's verified rule stops at `epoch_day`). The `epoch_*` transforms are
     format 1.1 (`DL/include/storage/ducklake_partition_data.hpp:20-31`); DuckLake has no truncate
     transform.
3. **The partition values are DuckLake's.** Every row's value is computed with DuckLake's own
   `DuckLakePartitionUtils::ApplyPartitionTransform` for the table's current spec
   (`DL/storage/ducklake_partition_data.cpp`), bound in the importing session as for any DuckLake writer:
   089's `partition_by` in phase B, the `COPY`'s expression in phase A. E7 measured two consequences on the
   DuckDB 2.0 pin:
   - TIMESTAMP_NS (`datetime2(7)`) is rounded to µs, half up, before `date_diff` / `month()` / `CAST AS
     DATE`, so a value in the last 500 ns of a period gets the next period's value. Those rows go to a file
     of that value: one extra small file per affected SQL Server partition, and only when such rows exist;
   - TIMESTAMPTZ transforms follow the session's `TimeZone`, which DuckLake does not normalise (the same
     instant is month 672 under `UTC` and 673 under `Europe/Berlin`). The docs say that a `datetimeoffset`
     column's partition values depend on the importing session's TimeZone, as for any DuckLake writer, and
     that an import from another TimeZone writes that TimeZone's values.

   The importer never declares a value DuckLake would compute differently. At this DuckLake pin only
   `bucket` values prune files (`DL/storage/ducklake_metadata_manager.cpp:2183-2258`), so temporal values
   shape grouping and compaction, not results; temporal pruning comes from min/max statistics.
4. **The sort order, proposed**: the clustered rowstore key, in `sysiscols` order and direction, becomes
   `SORTED BY (k1 [DESC], …)`, and an ordered CCI's `ORDER (col)` becomes `SORTED BY (col)`. Both apply
   **only when every key column orders in DuckDB as it does in SQL Server**: mssql's `OrdersLikeDuckDB`
   allow-list (numeric, bit, date/time). A string key under a collation gets no DuckLake sort, and the
   reason is recorded. The reason: the export writes in the table's DuckLake sort order (089 `order_by`)
   and DuckLake's compaction re-sorts by it (`DL/functions/ducklake_compaction_functions.cpp:718-726`). A
   collation's order is not DuckDB's, so a sorted file would interleave the rows of neighbouring pages and
   blow up the page map (D6). For a proposed clustered key the physical leaf-chain order already
   satisfies the sort, so nothing is sorted. An ordered CCI's `ORDER (col)` does not hold row by row
   inside a rowgroup (VertiPaq reorders the tuples), so that proposal makes the export sort each rowgroup
   and lose 088's tuple runs; DQ-25 asks whether to propose it. Heaps get no sort.
5. **The file layout is DuckLake's**: files are split by DuckLake partition value (a DuckLake file has one
   partition value set), written in the DuckLake sort order and cut at DuckLake's `target_file_size`.
   They are never split per SQL Server partition; those are the export's work units only (D4). A page
   whose rows have several DuckLake values gets one run per value.
6. The decision and its source are recorded in `__mssql_backup.tables`; E7's derivations are the test
   expectations (§ Testing).

### D6. Inlining off, and the importer's compaction

**Table options instead of lake options.** Every table the importer creates, mirrors and side tables
included, gets `data_inlining_row_limit = 0` and `auto_compact = false` in its `CREATE … WITH (…)`. Table
options are accepted at CREATE (`DL/storage/ducklake_table_entry.cpp:1378-1405`, scope checked by
`DL/common/ducklake_util.cpp:780-788`). `ducklake_set_option` would refuse a table or schema that is
transaction-local (`DL/functions/ducklake_set_option.cpp:96-105`), and these all are.

The lake-wide defaults stay as the user set them, for two reasons:

- the user's own tables keep inlining;
- a global `auto_compact = false` also turns off `ducklake_delete_orphaned_files`
  (`DL/functions/ducklake_cleanup_files.cpp:87-88`).

**Why inlining 0.** `AppendFiles` never inlines, but some writes inside an import are native:

- phase A's page map and provenance inserts, and 023's native `DELETE` fallback;
- `ALTER` on a table with inlined data from the same transaction is refused
  (`DL/storage/ducklake_table_entry.cpp:1473`);
- a delete of up to `limit` rows would become an inlined file deletion
  (`DL/storage/ducklake_delete.cpp:523-537`) instead of a delete file the importer can merge.

**Why auto_compact off.** `CHECKPOINT` (`DL/storage/ducklake_checkpoint.cpp:20-24`) and the lake-wide
compaction calls skip such a table. So does an explicit `ducklake_merge_adjacent_files('lake', 't')`: the
check sits inside the per-table loop of the one-table form as well
(`DL/functions/ducklake_compaction_functions.cpp:893`). A compaction rewrites files and moves positions,
so only the importer may start one.

**`ducklake_mssql_backup_compact(lake [, schema, table], small_file := target/8, delete_threshold := …)`.**
Per imported table, one transaction each, because compaction cannot share a transaction with changes
(`DL/storage/ducklake_transaction_state.cpp:421-423`):

1. `ducklake_set_option(…, 'auto_compact', true, table_name := t)`;
2. `ducklake_merge_adjacent_files` / `ducklake_rewrite_data_files`;
3. the option set back to false.

Options are written straight to the metadata and are not "changes"
(`DL/storage/ducklake_transaction.cpp:1680-1685`), so the transaction stays a compaction, and the user sees
no net option change. `merge_adjacent` keeps row order when the files are adjacent and no DuckDB-dialect
sort applies (`DL/functions/ducklake_compaction_functions.cpp:727-745`). D5's sort rule keeps it that
way. Only ties of a non-unique key at a page boundary can split a run, which is harmless.

**The page-map rebuild.** After any compaction, the importer's or one a user forced by flipping the option,
`page_map` rows that name files no longer live are stale. They are rebuilt before they are used (023 §D3):

- for each live file of an imported table that has no page-map rows, read `filename, file_row_number,
  _mssql_page` (DuckLake exposes both virtual columns; `test/sql/virtualcolumns/`); for a columnstore
  table, whose compressed rows have no page, read the (rowgroup, tuple) out of `_mssql_rid` (088 D3)
  instead;
- group consecutive equal pages (consecutive tuples of one rowgroup) into runs, take each run's `au_id`
  from the old page-map rows of the same pages, write page-map files, and delete the stale rows, all in
  the same commit.

The rebuild is exact whatever the row order (a page may get several runs), and its cost is one column of
the compacted files. `ducklake_mssql_backup_compact` runs the rebuild as its own follow-up transaction, and
every import checks for stale rows first, so a foreign compaction is caught too. When to compact is a
policy (024 §D5): files below `small_file`, and files with more than `delete_threshold` of their rows
deleted.

### D7. The commit, concretely

**DuckLake calls made by the apply** (public classes of the compiled-in DuckLake; the sources are never
edited):

| call | for |
| --- | --- |
| `DuckLakeTransaction::Get(context, catalog)` | the caller's lake transaction (`DL/include/storage/ducklake_transaction.hpp:223`) |
| `AppendFiles(table_id, vector<DuckLakeDataFile>)` | every data, page-map, mirror, side-table and provenance file (`:248`) |
| `AddNameMap(…)` | name-mapped files, unless they carry field ids (`:254`) |
| `SetCommitMessage(DuckLakeSnapshotCommit)` | author, message, extra_info (`:300`) |
| (023) `AddDeletes`, `DropFile`, `DuckLakeDeleteFileWriter::WriteDeleteFileWithSnapshots`, `DuckLakeDeleteFilter::ScanDeleteFile` | positional deletes from the page map |

**What DuckLake writes at commit.** `Commit()` → `FlushChanges()` → the commit loop
(`DL/storage/ducklake_transaction_state.cpp:2648-2710`). Each attempt increments `snapshot_id` once and
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
files, P = page-map, mirror and side files. Then:

- rows ≈ T·(2C + 6) for DDL and stats, + F·(1 + C) for files and their column statistics, + P·8, + (name
  maps) T·(1 + C), + a handful.

| scenario | T | C | F | P | rows (with name maps) | path |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| A: 1 TB, 2,000 tables, full | 2,000 + ~15 | 43 | ~4,000 | ~4,200 | ≈ 390 k (≈ 480 k) | ~185 k DDL rows as ~185 `VALUES` statements in a few `mssql_exec` batches; ~200 k file rows as bulk loads |
| B: 50 GB, 200 tables | 215 | 43 | ~300 | ~400 | ≈ 35 k (≈ 45 k) | seconds |
| C: one 2 TB table, 512 MB files | 16 | 30 | ~4,000 | ~4,000 | ≈ 160 k | mostly appender rows |

Research-ducklake-import's "~80 k rows for 2,000 files × 40 columns" counts files only. The DDL of 2,000
wide tables more than doubles it. **Not measured** (E8 was not run): the budget is ≤ 30 s for scenario A
against a local server, and task 12 measures it. The commit holds the catalog's snapshot lock for its
duration, so other writers to the lake wait. A full import is expected while the lake is otherwise quiet;
023's differentials are small (023 §D9). If the commit proves slow, the fix is the one research §4.2
names: extend the server-side apply to DuckLake's staged `DELETE_FILE` / `DROPPED_FILE` / `NAME_MAP`
tables (`DL/include/storage/ducklake_staged_commit.hpp:39-58`).

**Retries.**

- The loop checks for conflicts, then retries a retryable metadata error.
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
  `recovery_fork_id` (NULL until 085 locates it);
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
- **The run directory** `<data_path>/__mssql_backup/runs/<BackupSetGUID>/<run>/` holds `plan.json`, the
  per-unit manifests, 023's side inputs and a status file. The data files are written at their final paths,
  because registration is zero-copy.
- **Resume** (`resume := true`): when a run of the same file planned at the same plan id exists, the
  export skips the units whose files exist with their recorded sizes, and writes the rest.
  - If the plan id no longer matches (the lake moved), a new run starts and the old one's files are
    orphans.
  - The apply refuses with "the lake moved since planning (S0 → S1, import X); run again" when it sees
    the same thing.
- **A commit that succeeded unseen**: the BackupSetGUID is in `imports`, so `if_applied := 'skip'` returns
  that snapshot.
- **Orphans**: files of runs that never registered. `ducklake_mssql_backup_cleanup_runs(lake, older_than
  := INTERVAL 7 DAY, dry_run := true)` deletes the run directories and the unregistered files under the
  `mssql-<guid>-<run>-` prefix, and never a file DuckLake lists.
  - **Hazard**: DuckLake's own `ducklake_delete_orphaned_files` deletes them as well, after
    `delete_older_than` (2 days by default, `DL/functions/ducklake_cleanup_files.cpp:54`). An import that
    runs longer than that grace can lose its early files to a user's cleanup.
  - The apply's existence check fails closed, and the docs say so.
- **After the commit**, a `ClientContextState::TransactionCommit` hook (called only after a successful
  commit, `DD/src/transaction/transaction_context.cpp:81-83`) drops the temp manifest table and the run's
  scratch files. 024's automatic policy uses the same hook.

### D10. Progress and reporting

- **The export statement** implements `table_scan_progress`: pages written / pages planned. DuckDB's
  progress bar therefore covers the long phase, and the registration is short.
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
    owner's no-dirt decision (R§4). The reader's default for browsing stays open (DQ-13).
  - The counterfactual that imports it anyway (Testing, 6) is a test-only switch,
    `MSSQL_DUCKLAKE_TEST_ALLOW_DIRTY=1`, never a user option.
- **With 090**: the export replays every tail transaction and undoes the open ones (E6: exact on three
  loaded fulls, LOBs included), with `recovery := true`. The **pending set** comes from
  `mssql_backup_pending_pages` (090), from the same cached recovery: the pre-undo images of every page undo
  changed, and of every page an open transaction logged a row operation on, with the transaction ids
  (E6 §6.2). The wrapper writes it as a Parquet file and the apply registers it as
  `__mssql_sys.pending_pages`; 023 resolves it at the next file. A table recovery cannot cover is
  `unrecoverable`, and the file is refused naming it (090 D7).
- **The watermark for the next file** is W = min(FirstLSN, CheckpointLSN), with LastLSN recorded beside it
  (E6 rule 7 needs it).

## Enforcement & security

- **Fail closed.** These failures name the file, the GUIDs and the lake snapshots:
  - a header check or a plan-id mismatch;
  - a manifest file that is missing or the wrong size;
  - a unit counted twice;
  - a partition value that disagrees with the file's min/max;
  - a different source database.
- **The backup is untrusted input.** The reader bounds every length (mssql §9). The importer also checks
  every manifest: the run counts must sum to the file's row count, and the runs must not overlap.
- **Keys.** The import has no key parameter. The reader selects a `mssql_backup_key` secret by scope, then
  by thumbprint (087). Secrets are per instance, so the side connections see them; verify this for
  temporary secrets. Provenance records the thumbprint only, and the generated SQL and the logs carry no
  secret.
- **What the lake exposes.** The lake holds the database's data. `__mssql_sys` holds catalog rows; module
  source (procedures, functions, triggers) is not mirrored, only view text and partition boundaries
  (DQ-11). `pending_pages` holds uncommitted rows until 023 resolves them (DQ-09).
  Type-101 pad bodies never reach the lake (the reader skips them, E2 R10).
- **TDE backups** are refused until E10 reads their log tail: the reader reports `fuzzy_status =
  'unknown'` for every table of a TDE backup (087 D9), and no dirt may enter the lake.
- **Encrypted lakes** (DuckLake `encrypted`) are refused in v1. The export cannot encrypt with DuckLake's
  per-file keys, and `ducklake_add_data_files` refuses them too
  (`DL/functions/ducklake_add_data_files.cpp:45-48`).
- **Version mismatches.**
  - A ducklake bump re-audits the apply (it mirrors `AddWrittenFiles`, and in 023 `FlushDelete`), next to
    spec 014's closed list.
  - The manifest carries the reader's version; a run whose workers report different mssql versions is
    refused.
- **Concurrent importers** on one lake: the plan id and DuckLake's conflict check (two transactions
  creating the same schema or table conflict, `DL/storage/ducklake_transaction_state.cpp:191-205,265`) make the loser fail
  cleanly. The run id keeps their files apart.

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
- locators: heap `_mssql_rid` = `%%physloc%%` (RESTORE keeps physical pages), clustered `_mssql_page` =
  the physloc page;
- also: row counts, and the lake schema against the expected type map;
- one macro, `mssql_backup_oracle(lake, version, restored_db)`, returns the differing keys per table.

**Lake against lake**: phase A and phase B over the same file give equal rows, mirrors and page-map runs,
up to file names.

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
  - every file's partition value equals DuckLake's own value for each of its rows, computed in the
    importing session: the `datetime2(7)` rows at b − 100 ns and b − 500 ns land in the next period's file
    (E7§3); the `datetimeoffset` table imported under `TimeZone = 'UTC'` and under `'Europe/Berlin'` gets
    each session's values (E7's `duck_check_berlin.csv`), and both lakes equal RESTORE;
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

1. Inlining left at 10: the page-map and provenance inserts are inlined, and 023's `ALTER` in the next
   commit is refused (`statement error`).
2. `auto_compact` left true: a `CHECKPOINT` compacts an imported table. Without the rebuild, the next
   differential's positional deletes hit other rows and the oracle reports them. With the rebuild, 0.
3. A sort on a string key under a case-insensitive collation: after `ducklake_mssql_backup_compact` the
   page map has several runs per page (measured). That is why D5 withholds the sort.
4. No run id in the names: two concurrent imports overwrite each other's files (a C++ test).
5. The plan-id check off: an import planned before another one committed registers stale DDL. With the
   check on, it is refused.
6. `MSSQL_DUCKLAKE_TEST_ALLOW_DIRTY=1` on a loaded full: dirty keys appear (E6: 7,516). That is why the
   import refuses, and why the switch is test-only (it replaces the drafted `fuzzy := 'allow'`).

**The commit**: scenario A against the SQL Server catalog, recording the statements, the rows and the
time, with `MSSQL_DUCKLAKE_STRICT_BATCH=1`.

**Failure**:

- a kill between export and commit, after which resume reuses the files;
- an injected commit failure, after which the files are kept and the retry succeeds;
- an already-applied file, which is skipped and returns its snapshot id;
- `cleanup_runs`, which deletes only unregistered run files;
- a manifest naming a deleted file, which is refused.

## Alternatives considered

- **`INSERT … SELECT FROM mssql_backup_scan` in one transaction** (research §4.3 option A): the lake
  transaction lasts the whole import, a rollback deletes every file, big tables run one after another, and
  page positions are unknown.
- **Writing DuckLake's metadata rows directly**, as `iceberg_to_ducklake` does: format 0.4 and an empty
  catalog only, `row_id_start` left NULL, no conflict checks, and hand-written T-SQL to re-validate at
  every DuckLake bump.
- **A `CALL` table function doing the DDL through the Catalog API**: binding `PARTITIONED BY` / `SORTED BY`
  / `WITH` by hand, where the SQL is the stable contract (D1).
- **`ATTACH` the backup, then `COPY FROM DATABASE`**: one UNION ALL, big tables serialised, no positions.
- **One snapshot per table**: against the owner's decision.
- **Setting the options lake-wide**: changes the user's tables, and a global `auto_compact = false` stops
  orphan cleanup (D6).

## Open questions for the owner

All are in [decisions.md](https://github.com/hugr-lab/mssql-extension/blob/main/specs/085-backup-reader-core/decisions.md), merged with the mssql series and ordered by impact:

| # | question | DECISIONS |
| --- | --- | --- |
| 1 | the run id in file names | DQ-34 |
| 2 | Parquet field ids or name maps | DQ-24 |
| 3 | service column types (`_mssql_rid` BLOB or STRUCT; `_mssql_lob` container) | DQ-02, with DQ-01 for the LOB bytes |
| 4 | nullability | DQ-23 |
| 5 | the fuzzy default | settled: refuse (no dirt, R§4); the user option is gone (D1, D11) |
| 6 | one source database per lake in v1 | DQ-22 |
| 7 | `bucket` overrides | DQ-26 |
| 8 | proposing `SORTED BY` only for keys that order like DuckDB's | DQ-25 |
| 9 | what the mirror holds (`sysobjvalues` without module source) | DQ-11 |
| 10 | encrypted lakes | DQ-28 |

Added by the consistency pass: DQ-05 (a phase-A lake carries no LOB maps until 089 PR 1).

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
| 4 | Partition and sort proposal from 085's `partitioning` (E7's rule: inner partitions, ticks, RANGE LEFT and RIGHT, `datetimeoffset` in UTC instants), coarsening, overrides, the `OrdersLikeDuckDB` rule, recording; the partition values through DuckLake's `ApplyPartitionTransform` in the importing session, the side connection's TimeZone; E7's expectations as tests | 3 d |
| 5 | Phase A: `COPY` per table and partition, the mirrors, `ducklake_add_data_files`, the page map from `file_row_number`, provenance, the commit message | 4 d |
| 6 | The oracle harness: the RESTORE side, the canonical rows macro, the locator checks; quiet-full scenarios | 5 d |
| 7 | Phase B registration: manifest → `DuckLakeDataFile`, `AppendFiles`, name maps (or field ids), partition values and their check, skipped statistics, the importer's own files | 5 d |
| 8 | Phase B export: the `ducklake_mssql_backup_export` wrapper (side connection, Parquet settings, per-unit manifests), external-worker manifests and their validation | 4 d |
| 9 | Failure handling: resume, plan-id refusal, `if_applied`, `cleanup_runs`, the post-commit hook | 3 d |
| 10 | Progress (`table_scan_progress`), logging, the summary rows | 1 d |
| 11 | Compaction: `ducklake_mssql_backup_compact` (the option toggle), the page-map rebuild function (shared with 023), counterfactuals 2 and 3 | 4 d |
| 12 | The commit: the 2,000-table measurement on the SQL Server catalog, strict-batch shapes added to spec 014's list, the lake-vs-lake check of phase A against B | 3 d |
| 13 | A loaded full: `fuzzy` before 090; the pending set registered after 090 | 2 d |
| 14 | The docs page (`website/docs/`), the README index row | 2 d |
| | **total** | **45 d ≈ 9 wk** |

Phase A (tasks 1–6, 10, 14) is 24 d, **about 5 weeks**, needing 085 and 086. Phase B (tasks 7–9, 11–13)
is 21 d, **about 4 weeks**, needing 089. That is the README's 7–9 weeks, depending on how far the harness
work (task 6) overlaps the rest.


> **Owner, 2026-11-05 (DQ-02):** every `_mssql_*` service column is an opaque `BLOB` serialized and
> read only by the import code; each starts with a format/shape byte. `_mssql_page` is big-endian
> `u16 file ‖ u32 page` so min/max still prune. No `_mssql_rowgroup`.
