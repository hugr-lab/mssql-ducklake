# Spec 023: applying a differential or a later full on top of the lake

- **Status**: draft
- **Date**: 2026-11-05
- **Author**: vgsml, with Claude

## Summary

After 022's first import, the lake is the base. Each later file is applied with nothing but the lake and
that one file, never the old full and never an intermediate differential. A file can be:

- a differential;
- a periodic new full;
- later, a log backup.

Each applied file is one snapshot, registered by the same plan → export → register pipeline as 022. What
this spec adds is the apply algorithm, which E1 proved on 21 states over 5 chains and E6 made exact under
load:

1. **Chain checks.** A file is validated against the lake's own provenance first.
2. **The catalog first.** The `__mssql_sys` mirror is updated, and its diff becomes DuckLake DDL inside
   the same commit.
3. **Rows by page.** The rows of every changed, freed or dead page are deleted from **the table that held
   the page**, which the base allocation (IAM) images and the mirror name, by DuckLake's own `DELETE …
   WHERE _mssql_page IN (…)`. Then the pages' new rows are inserted.
4. **LOBs.** A LOB only partly in the file is rebuilt from the lake's fragment map.
5. **Pending and stale pages** carry what a loaded file could not settle to the next file, which heals
   it.
6. **The rest.** Partition maintenance (E7's rules; a SWITCH by lake-side copy) and columnstore deletes are
   handled in the same commit.

Owner decisions are cited as DQ-nn, from one list merged across both series:
[decisions.md](https://github.com/hugr-lab/mssql-extension/blob/main/specs/085-backup-reader-core/decisions.md).
The layout of every file the apply writes is DuckLake's (owner, 2026-11-05; 022 D5, DQ-10).

## Problem

DuckLake has no notion of a page. A differential carries only the extents changed since its base full
(E2 R1: DCM ∩ GAM-allocated). It does not carry the evidence of a delete, only the allocation pages that
show it (E2 R2). And it rewrites whole pages whose rows are mostly unchanged: in E1, only 3–68 % of a
differential's pages had changed at all. Getting the lake to equal the database at each backup took
twelve rules, each shown necessary by a failure:

- the PFS for frees;
- delete by page, in the table the base IAM image names (E1 4.3);
- the catalog first, with `sysseobjvalues`;
- the fragment map for `.WRITE`;
- the chain checks;
- the pending pages, with their allocation check;
- CheckpointLSN ordering;
- ignoring tail records already in the lake;
- skipping tail transactions whole;
- keeping an unresolved LOB's map;
- the R3 liveness test;
- the ownership rule after a SWITCH (E7).

Three ways of carrying them into DuckLake fall short:

- A `DELETE … WHERE _mssql_page IN (…)` on every table reads `_mssql_page` of every file of every table,
  so its cost grows with the lake, not with the change. Each page's delete must go to the one table that
  held it.
- DuckLake's `MERGE` allows one UPDATE/DELETE action (`DL/storage/ducklake_merge_into.cpp:11-23`).
- A transaction per table breaks the one-snapshot rule.

Paths as in 022: `DL/` = `ducklake/src/`, `DD/` = `duckdb/`, `MD/` = `src/`.

## Design

### D1. The surface

The same PRAGMA as 022 decides from the header which kind of file it holds. A strict form refuses
anything but a differential:

```sql
PRAGMA ducklake_import_mssql_backup('lake', 's3://bak/sales_full_2026-12-01.bak');      -- a later full
PRAGMA ducklake_import_mssql_diff_backup('lake', 's3://bak/sales_diff_2026-11-05.bak');  -- a differential only
```

These options are added to 022's:

| option | default | meaning |
| --- | --- | --- |
| `mode` | `'auto'` | `'incremental'` applies the changed pages (D3); `'rebase'` (fulls only) reads every page of the full whatever its LSN (D10) |
| `loaded_diff` | `config`, else `'skip'` | for a loaded differential (with 090), passed to the reader's parameter of the same name (090 D4): `'skip'` skips whole every tail transaction that touches a page the file lacks (the owner's variant 1); `'partial'` redoes what it can (fewer stale keys, but interim duplicates and torn LOBs, E6 §3.4), offered as a non-default, documented option (DQ-17, accepted) |
| `foreign_writes` | `'error'` | what to do when a table was written by a non-import snapshot since the last import (D12) |
| `partitioning`, `sorting` | derived | as in 022, for tables this file creates |

### D2. Chain validation and ordering

The rules below run in the callback at S0, and again at S1 in the transaction's first statement, the
validation (022 D2.3), before any `DELETE`. L is the last applied import
(from `__mssql_backup.imports`) and B is the lake's diff base. The reader's `mssql_backup_chain_check`
(089) evaluates C1, C3, C4 and C5 from the file alone; the importer adds C2 (every import, not only the
last), C6 and C7.

| # | rule | refusal names | source |
| --- | --- | --- | --- |
| C1 | same database: FamilyGUID equal (085's `family_guid`, `dbi_familyGUID`), and RecoveryForkID equal to the lake's current fork. FamilyGUID survives a restore, so a restored copy elsewhere passes it; the fork id is what tells the two lines apart. HEADERONLY exposes RecoveryForkID, FirstRecoveryForkID and ForkPointLSN; 085 task 3 locates them, and 089's chain check compares the file's fork with the lake's. **Fails closed**, by the rule 089 D7 states: (a) while the file's fork id cannot be read, every apply by LSN is refused, a differential or a full applied incrementally alike, and the way out is a full imported as a new base (`mode := 'rebase'`), which takes no LSN from the lake's chain: a rebase lets fulls in, but differentials stay refused until 085 reads the fork id; (b) when the lake has no fork recorded but the file's can be read, the apply is refused, and a rebase on a full records the fork | both GUIDs; "the database was restored or forked: apply a full with `mode := 'rebase'`"; (a) "the recovery fork is not read yet: a full can be applied with `mode := 'rebase'`, and differentials are refused until the reader reads the fork id"; (b) "the lake records no recovery fork: apply a full with `mode := 'rebase'` to record it" | RESEARCH §3 (FamilyGUID); after a fork the LSNs restart below the lake's, so the LSN filter would be wrong |
| C2 | not yet applied: BackupSetGUID ∉ `imports` | `if_applied := 'skip'` returns that snapshot | 022 D9 |
| C3 | order by **CheckpointLSN**: f.CheckpointLSN > L.CheckpointLSN | both LSNs and L's snapshot | E6 §3.4: a transaction open across two files gives both the same FirstLSN, and E1's FirstLSN check refused a valid D5 |
| C4 | a differential's base: f.DifferentialBaseGUID = B.guid and f.DifferentialBaseLSN = B.lsn | "differential base 63da… is not the lake's base c53c…; apply full 63da… first" when known | E1 rule 1; forcing D3 onto a lake that skipped F1 corrupted 3 tables |
| C5 | a full is accepted after C1–C3; a **non-copy-only** full becomes the new base (B := f) and starts a generation (DQ-16, 024 D1); a copy-only full changes neither the base, the DCM nor the generation | — | RESEARCH §6.2, E6 Q1 |
| C6 | a lake whose first import was a copy-only full: B := the base that file's header reports, if it reports the database's current diff base (**to verify** on the E1 corpus); otherwise B is unknown and differentials are refused until a non-copy-only full is applied | "no differential base yet" | DQ-15 |
| C7 | a striped set is complete (FamilyCount files, one MediaSetId; the reader refuses otherwise) and the family is recorded | — | E2 R9 |

What the reader is given (089, 090):

- `since_lsn := W`, with W = min(L.FirstLSN, L.CheckpointLSN) (E1 rule 2, E6);
- `reflected_lsn := L.LastLSN` (E6 rule 7);
- `base := '<run>/base/'` (089 D10), written by the callback from the lake at S0:
  `catalog/<base_table>.parquet` (the `__mssql_sys` mirrors), `allocation.parquet` (written from
  `alloc_pages` column for column, `(file_id, page_id, page_type, page_lsn, au_id, image)`: the reader
  checks the base files' schemas exactly; its IAM images also name the AU that owned each page, D4),
  `pending.parquet` (`pending_pages`) and `lob.parquet` (D6);
- `recovery := true` and `loaded_diff` (the option of D1; 090 D4).

The stale page set is not given to the reader: it stays in `stale_pages`, and the apply checks its healing
against the reader's output (D7).

A differential needs no intermediate file: differentials are cumulative since their base, so a later one
can follow an earlier one directly, as D2 followed F0 with D1 skipped (E1 chain B).

### D3. The apply, in order, in one transaction

The callback plans at S0 through a side connection, as in 022. A differential also needs the lake's
mirror as its catalog: a diff does not carry the unchanged catalog pages. So the callback:

1. writes the base side files of D2 except `lob.parquet` into `<run>/base/`, from `__mssql_sys` at S0;
2. asks the reader for the file's catalog over them (`mssql_backup_tables(file, base := …)`, 089) and diffs
   it against the registry by object id and column id (D5);
3. asks for the delete and move evidence, `mssql_backup_changed_pages(file, since_lsn := W, base := …,
   reflected_lsn := …, recovery := true, loaded_diff := …, expand := true)`, one row per page, and
   splits it:
   - the rows of the delete-set kinds with `holds_rows` (D4) go to the per-table delete sets, each
     page's table taken from its base owner `from_object_id` / `from_partition_number` and mapped to a
     lake table through the registry;
   - the `au_moved` rows with `holds_rows` feed the SWITCH copy's moved pages (D8) only;

   then writes `base/lob.parquet` for the rows the apply will delete (D6);
4. writes `plan.json`.

The expansion:

```sql
PRAGMA ducklake_import_mssql_diff_backup('lake', 's3://bak/sales_diff_2026-11-05.bak');
-- expands to (the side inputs are already in the run directory, read AT (VERSION => S0)), as inside an
-- explicit transaction; in autocommit the statements after the export run as one transaction on
-- ducklake_mssql_backup_commit's side connection, retried on a conflict up to
-- N = import.conflict_retries times, N+1 attempts (022 D1, D11):
CREATE TEMP TABLE __mssql_backup_manifest_<run> AS
    SELECT * FROM ducklake_mssql_backup_export('lake', '…', plan := '<run>/plan.json');
SELECT * FROM ducklake_mssql_backup_validate('lake', '…', plan := '<run>/plan.json');  -- S1 (022 D2.3)
ALTER TABLE lake.dbo.t_pk ADD COLUMN c_def INTEGER DEFAULT 42;       -- the DDL of D5, in order
CREATE TABLE lake.dbo.t_new (…) WITH (data_inlining_row_limit = 0, …);
DROP TABLE lake.dbo.t_drop;
DELETE FROM lake.dbo.t_pk WHERE _mssql_page IN                     -- D4, one per affected table
    (SELECT page FROM read_parquet('<run>/delete_pages.parquet') WHERE lake_table = 'dbo.t_pk');
INSERT INTO lake.dbo.m_arch SELECT … FROM lake.dbo.m WHERE _mssql_page IN (…);  -- a SWITCH (D8)
DELETE FROM lake.dbo.m WHERE _mssql_page IN (…);
SELECT * FROM ducklake_mssql_backup_apply('lake', '…', manifest := …, plan := …);  -- the new files
```

The algorithm (the numbered steps are the order of effect; E1 §2, E6 §3.1, E7§5):

1. **Pending pages first** (with 090; D7): the reader resolves each pending page against the file *before*
   its own redo, so a tail record on P finds its base. Applied images join the file's page store as if the
   file carried them.
2. **Export**: the rows of every page in the changed set C are written as new data files, in the lake
   table's current DuckLake layout (spec, sort order, file size; 022 D5). C is:
   - pages with LSN > W;
   - applied pending images;
   - pages formatted in the tail (E6 §3.1).

   The rows come only from PFS-allocated data pages whose header names a known in-row allocation unit,
   skipping ghost and stub records (E1 rule 4, E2 R3/R4); a row's table is its AU's owner in the file's
   catalog. The same holds for the mirrors' catalog pages. The reader also reports, through
   `mssql_backup_changed_pages`, `mssql_backup_pending_pages` and `mssql_backup_tables`:
   - the unallocated ranges U (`freed`): per interval from the file's PFS / GAM, else the lake's
     allocation image, plus every page past the file's `Size`;
   - the dead allocation units and rowsets A: `dead_au` (type 0 / owner 0), `gone_au`, and for a
     columnstore `dead_rowgroup` (a TOMBSTONE or vanished rowgroup, 088) (E2 R5, E5 §3.7, E7 s10);
   - the moved allocation units M (`au_moved`, E7);
   - the new pending and stale sets;
   - the skipped transactions, and the status per table.
3. **Catalog first** (E1 rule 6): the DDL statements above. The mirror tables are applied exactly like
   user tables in steps 4–6, because their rows live on pages too.
4. **Delete by page, in the table the base IAM image names** (E1 4.3): the delete set is D = C ∪ U ∪
   (the pages of the AUs in A, from the base IAM image; a dead columnstore rowgroup by its rowgroup key,
   D9), data pages only (D4). Each page of D is deleted from the lake table of the AU that owned it in
   the previous file: the reader names that AU from the base IAM image (`base_au_id`: an extent, or a
   mixed extent's single-page slot) and its owner from the base mirror (`from_object_id`), and the
   registry maps the owner to its lake table (D4). A page freed or reallocated to another AU is deleted
   from its old AU's table only; a page that changed inside its AU, from that AU's table; a page no
   reachable base IAM chain lists held no lake rows. A page that moved from a truncated table to another
   is deleted from the truncated table. A dropped table is a `DROP TABLE` (step 3). Per affected table,
   one `DELETE … WHERE _mssql_page IN (…)` (D4).
5. **Ownership** (E7's rule, D8): for every AU in M, the lake rows on its pages in the base IAM image
   (columnstore: a compressed rowgroup's rows by the moved rowset id in their rowgroup key; a delta store
   moves as an AU), minus the pages of D (step 4 deletes those and step 2 re-read them under their new
   owner), move to the owner the file's catalog gives:
   `INSERT INTO target SELECT … FROM source WHERE _mssql_page IN (…)`, then `DELETE FROM source WHERE
   _mssql_page IN (the AU's pages)`. A change of `numpart` alone moves nothing.
6. **Insert**: `AppendFiles` for the new files, after every `DELETE` of steps 4 and 5, so a page's new
   rows are never hit by the delete of that page (D4).
7. **Side tables**:
   - `pending_pages` := (old − resolved) + new;
   - `stale_pages` := (old − healed) + new;
   - `alloc_pages`: the images `mssql_backup_allocation(file, …, recovery := true, loaded_diff := …,
     reflected_lsn := …, base := …)` returns (for a loaded file, those of the recovered state, 090)
     replace the base's; the image of every page the file shows freed, or carries as a non-allocation
     page, is dropped; a full replaces the whole set. So the next file's deletes see this file's owners.
8. **Provenance and registry**: the `imports` row; `tables`, `columns` and `views` updated; the commit
   message (022 D8).

Page splits, moves, forwarding, rebuilds, shrinks, SPLIT / MERGE / TRUNCATE of partitions and pages
reused across tables need nothing special: every page involved is in C, U or A, so its old rows go and its
new rows come (E1 rule 5, E2 §2, E7§5). Only a SWITCH needs step 5. Re-processing a page is idempotent
(E1 rule 2).

### D4. Deletes by `_mssql_page`

**The delete sets are computed in the plan, at S0.** D is known from the file's page index and the mirror
diff before any row is decoded, through `mssql_backup_changed_pages(file, since_lsn := W, base := …,
expand := true)` (D3); the moved pages (D8) likewise.

- **The kinds.** A lake table's part of D is the rows

  ```sql
  kind IN ('changed', 'freed', 'dead_au', 'gone_au', 'dead_rowgroup', 'pending_applied')
    AND holds_rows AND from_object_id = <its object id>
  ```

  `dead_rowgroup` is a compressed rowgroup as its `02` rowgroup key, or a TOMBSTONE's delta store as
  its AU's base IAM pages in the `01` form (D9). `pending_applied` is 090's kind for an applied
  pending image: one row per page, never also `changed`. `pending_discarded`, `pending_kept` and
  `stale` are not in D. The `au_moved` rows feed the SWITCH copy only (D8).
- **Data pages only** (`holds_rows BOOLEAN`, 089; NULL without `base`, which an import always passes). It
  is true when the page's base AU is the `IN_ROW_DATA` AU of a heap or a clustered index, or a delta
  store's, or when the page is a rowgroup key (D9); false for LOB, row-overflow, nonclustered-index and
  allocation pages. A LOB or row-overflow page reaches its rows through the LOB map (D6, 089); index and
  allocation pages hold no lake rows.

1. Each page of D gets the AU that owned it in the previous file, read from the base IAM images
   (`alloc_pages` at S0, passed as `base/allocation.parquet`). 089 reports it as `base_au_id`, next to
   the current owner `au_id`, and that AU's owner in the base mirror as `from_object_id` /
   `from_partition_number`. The importer takes the table from `from_object_id` and maps it to a lake
   table through the registry; it does not re-derive it from `base_au_id` through the mirror.
   `base_au_id` stays for diagnostics and the SWITCH's page lists (D8).
   - Only the IAM pages on the AU's first-IAM chain in the base mirror's `sysallocunits`, and live in
     the base GAM / PFS, count; a page two such chains claim is the reader's named error (089). A page
     no reachable base IAM chain lists held no lake rows and leaves the set.
   - A page whose base owner (`from_object_id`) is an object the file drops or re-creates leaves the
     set, because that lake table is dropped (D5). A TRANSFER's source and a table rewritten with a cast
     the pages did not get (D5) are dropped too, after the lake-side export that copies them: their pages
     leave the set, since no `DELETE` runs on a dropped table, but stay in `delete_pages.parquet`, which
     that copy excludes (D5).
   - A `changed` row is kept when its current owner or its base owner passes the import's `schemas` /
     `tables` filters (022 D1), so a page reused out of an imported table into a filtered-out one still
     deletes its old rows; `freed` and the AU kinds are never filtered.
   - This needs `alloc_pages` to describe the allocation the lake's rows reflect, which step 7 of D3
     keeps true; after a loaded file, through the images `mssql_backup_allocation` returns for the
     recovered state (090, to verify there). `mssql_backup_allocation` runs the attribution check
     (089): a page whose rows this file gives the lake (changed, rebuilt or `pending_applied`) that is
     not on its header AU's IAM chain, or is on more than one, in the images it returns is refused by
     name; the export repeats the check on the pages it writes. Either way the import refuses the file,
     naming it, so a missed delete is never silent, with rows from `mssql_backup_scan` (phase A) as
     with rows from the export.
2. The pairs `(lake_table, page)` of the rows that pass (`holds_rows` included) are written to
   `<run>/delete_pages.parquet` (an importer-private file, not a reader input), `page` in
   `_mssql_page`'s VARCHAR form (022 D3), as in `changed_pages`.
3. The validation, the transaction's first statement, re-validates S1 against S0 (022 D2.3) before any
   `DELETE`. The sets are values, not positions, so a compaction of the table's files between S0 and S1
   changes nothing.

**Running them, in the import's transaction.** Per affected lake table, the expansion runs DuckLake's own
delete:

```sql
DELETE FROM lake.s.t WHERE _mssql_page IN
    (SELECT page FROM read_parquet('<run>/delete_pages.parquet') WHERE lake_table = 's.t');
```

- DuckLake finds the positions and writes its own delete files: one cumulative delete file per data
  file, which supersedes the previous one and schedules it for deletion
  (`DL/storage/ducklake_metadata_manager.cpp:4581-4603`); a data file all of whose rows are deleted is
  dropped (`DL/storage/ducklake_delete.cpp:408-420`).
- A columnstore's delete-bitmap entries are separate `DELETE`s, one per rowgroup, by its rowgroup key and
  `_mssql_rid` (D9).
- **Order.** The validation comes first. The `DELETE`s, and a SWITCH's copy and delete (D8), run after
  it and before the new files are registered (the apply's `AppendFiles`, D3 step 6; phase A's
  `ducklake_add_data_files`), all in the import's single DuckLake transaction, so one snapshot holds both;
  a failure in any statement rolls the whole transaction back. Either registration gets the same
  bounded retry on a conflict at commit: in autocommit phase A's statements, `ducklake_add_data_files`
  included, run through `ducklake_mssql_backup_commit` as the apply does (022 D2.4, D11). A page's
  new rows carry the same `_mssql_page` as its old ones; the order is what keeps them from being
  deleted with them. Confirmed (mssql-ducklake, 2026-11-06): `DELETE` + `ducklake_add_data_files` in
  one transaction make one snapshot, and the new rows of a deleted page survive; task 5 keeps it as a
  test. The transaction holds the catalog's server transaction while the `DELETE` scans and the copy
  run (022 D1).
- **Idempotent.** A delete of a page whose rows are already gone matches nothing, so re-processing a page
  (E1 rule 2) and a delete-bitmap entry seen twice (D9) are harmless.
- **Resources.** The `DELETE`s, a SWITCH's copy (D8) and the lake-side exports (D5) are DuckDB
  statements: buffer-managed and spilling, they obey DuckDB's limits, plus the `operator_memory_limit`
  of `max_memory` / 2 the import sets on its side connections (the export's, and in autocommit the
  commit's; 022 D12's split); `max_memory` and `max_threads` do not bound them otherwise. The delete
  sets reach them as files, never as an in-memory list.

**The cost.** A `DELETE` by `_mssql_page` reads the `_mssql_page` column of the table's files, and skips
the files whose min/max of it cannot hold a page of the set; only the tables the delete sets name are
read. `_mssql_page` is a fixed-width hex VARCHAR (022 D3), so within a shape string order is page
order, and DuckLake builds a file filter for it from constants (`=`, `IN`, `BETWEEN`), where it builds
none for a BLOB (`DL/storage/ducklake_metadata_manager.cpp:1764-1765`; measured by mssql-ducklake, 022
D3). Files written in physical order hold contiguous pages (022 D4), so a changed page should touch few
files. Heaps, written in page order, should prune best; a clustered table, written in leaf-chain order,
less well once page splits have moved its leaf chain away from page order. A lake sort order (022 D5, or
a later DuckLake `SET SORTED BY`) scatters a page's rows over many files and makes a delete read more of
them: a cost, never a wrong row. Each touched data file's cumulative delete file is rewritten too (D11).
`skip_stats_columns` never lists `_mssql_page` (022 D3).

**What remains to verify** (022 task 11, task 6): that `IN (SELECT …)` prunes the VARCHAR column through
DuckDB's dynamic join filter, as mssql-ducklake measured for BIGINT. The file filter's order is answered
(022 D3): DuckLake runs it as string comparisons in the metadata database
(`DL/storage/ducklake_metadata_manager.cpp:1693-1745`), and on a SQL Server catalog this extension
stores every VARCHAR column, the min/max statistics columns included, as `COLLATE
Latin1_General_100_BIN2_UTF8`, which its comment says is what makes DuckLake's pushed min/max prune
correctly (`MD/include/mssql_metadata_manager.hpp:165-168`, `MD/mssql_metadata_manager.cpp:161`,
`MD/mssql_catalog_shape.cpp:833-847`). BIN2 compares by code point, so the fixed-width uppercase hex
orders as bytes; a test keeps it (022 task 11).

- If the `IN (SELECT …)` does not prune, the expansion passes the delete set as constants: an `IN`
  list, cut into chunks in page order, each with the `BETWEEN` bounds of its contiguous range, one
  `DELETE` per chunk: `WHERE _mssql_page BETWEEN '0100010000A000' AND '0100010000A0FF' AND
  _mssql_page IN ('0100010000A000', …)`. The rows are the same, and both terms are constants DuckLake
  turns into a file filter.
- Task 6 measures the files a one-page delete reads, after N differentials and after a
  `ducklake_merge_adjacent_files`.

### D5. Schema evolution inside the commit

The mirror's catalog is diffed against the registry by object id and column id (E1 rule 6; E2 §4 for how
each change shows on disk). Each change is either a native DuckLake ALTER with stable field ids, or a
rewrite inside the same commit. Table and column ids are kept wherever DuckLake allows: renames keep
them (`DL/storage/ducklake_table_entry.cpp:785-810,1478`), and a DROP + ADD makes new ones.

| SQL Server change | seen as | lake action | rewrite |
| --- | --- | --- | --- |
| new table | new object, rowsets and AUs | `CREATE TABLE`, with 022 D5's spec and the `WITH` options | no |
| dropped table | object row gone; AUs gone or dead | `DROP TABLE`; registry status `dropped` | no |
| dropped and re-created under the same name (new object_id) | old object gone, new one present | `DROP` + `CREATE`: the new lake table gets the creation-time proposal (022 D5); the old table's manual spec edits go with the old table | no |
| table rename (object_id stable) | sysschobjs name | `ALTER TABLE … RENAME`, but only if the lake name is still the one the importer last set; a manually renamed lake table is left alone, with a notice (DQ-27) | no |
| `ALTER SCHEMA … TRANSFER` | schema id changed; pages untouched | DuckDB has no SET SCHEMA (`DD/src/include/duckdb/parser/parsed_data/alter_table_info.hpp:72-92`): `CREATE` in the target with the current spec, rows moved by a **lake-side export** (below), `DROP` the source | yes (lake copy) |
| ADD COLUMN NULL (metadata-only) | new syscolpars and sysrscols rows | `ADD COLUMN` | no |
| ADD COLUMN NOT NULL DEFAULT k (metadata-only) | sysrscols status 0x200; k in `sysseobjvalues` by **hbcolid** | `ADD COLUMN c T DEFAULT k`: old files read k as the initial default (literal only, `DL/storage/ducklake_field_data.cpp:80-90`) | no |
| DROP COLUMN (metadata-only; bytes kept) | syscolpars row gone; sysrscols status 0x02, new rscolid | remove the partition or sort field on it first, that field only (022 D5; DuckLake refuses otherwise, `DL/storage/ducklake_table_entry.cpp:876-900`); `DROP COLUMN` | no |
| column rename | syscolpars name | `RENAME COLUMN` (field id kept); same manual-wins check as tables | no |
| int → bigint, decimal precision widening (size-of-data) | a new physical column, the old one kept as dropped | `ALTER COLUMN … TYPE` (a promotion DuckDB allows, `ducklake_table_entry.cpp:956-967,1106-1130`); its partition and sort fields stay, and under a `bucket` field whose hash changes DuckLake starts a new partition spec (022 D5) | every page is in the file anyway |
| varchar / nvarchar length change (metadata-only) | syscolpars length | nothing: the lake has no length; the registry is updated | no |
| other type changes: scale, int → varchar, … (size-of-data) | a new physical column | `ADD` under a temporary name, `DROP` the old (a partition or sort field on it removed first, 022 D5), `RENAME`; the rows are re-inserted from the file because every page changed | no lake copy |
| a type change DuckLake cannot express, where SQL Server did **not** rewrite the pages | the type changed, no new physical column | as for a TRANSFER source: `DROP` the old table and `CREATE` it with the new type and its current spec, less a partition or sort field the new type does not take (022 D5), the rows by a lake-side export with the cast (below) | yes (lake copy) |
| NULL ↔ NOT NULL | sysrscols 0x80 | nothing; lake columns are nullable (022 D3, DQ-23) | no |
| computed column | syscolpars computed flag | persisted: a plain column (size-of-data); not persisted: registry text only (DuckLake refuses generated columns) | no |
| PK / clustered index created, dropped or rebuilt; heap REBUILD; a new partition scheme | new rowsets and in-row AUs; every in-row page new; the LOB_DATA AU kept (E2 R6) | nothing structural: old pages are in A, new pages in C. LOB values are reused by root bytes (D6). **Partitioning and sort order are not changed** (022 D5) | no lake copy |
| a SQL Server upgrade adds a base-table column | the mirror's schema differs | `ADD COLUMN` on the mirror table | no |

A **lake-side export** reads the source rows from the lake at S0, in `filename, file_row_number` order,
less the pages this file deletes from the table (its delete set, D4) or moves out of it (D8):

```sql
COPY (SELECT … FROM lake.s.t AT (VERSION => S0)
      WHERE _mssql_page NOT IN (SELECT page FROM read_parquet('<run>/delete_pages.parquet')
                                WHERE lake_table = 's.t')
        AND _mssql_page NOT IN (<the table's moved pages>)
      ORDER BY filename, file_row_number) TO …
```

So it copies the table as it stands after this file's deletes: the rows of a page in the delete set are
gone or re-read from the file (D3 step 2), and the rows of a moved page go by D8. It runs in the export
phase (022 D2.2: a side connection, no lake transaction), writes new files in the source's file order, so
a page's rows stay together (D4), and those are registered like any export. A whole table is copied this
way, not by an `INSERT … SELECT` inside the import's transaction, which would hold the catalog's server
transaction for the whole copy (022 D1); a SWITCH's copy of the moved pages is that statement (D8).

**The one spec change** (022 D5), made before the ALTER. When the backup's catalog removes a column a
partition or sort field uses, by a DROP COLUMN or by a type change done as ADD / DROP / RENAME, the table
stays, so the importer removes only the field the change hits; the rest of the spec, manual edits
included, stays. A SQL Server re-create (DROP + CREATE of a table) re-creates the lake table, and the
new lake table gets the creation-time proposal; the old table's manual spec edits go with the old
table. A promotion in place keeps its fields. The removal is recorded in the commit message, `imports`
and `tables`, and a WARNING names it.

### D6. LOBs and the fragment map

Every row re-inserted carries a new `_mssql_lob` (022 D3). The reader (089) decides each value's source:

1. **The root is byte-identical to the lake's** (the same RID, or the same root bytes): the value and its
   map are copied from the lake. This also covers REBUILD, which keeps the LOB_DATA AU and does not rewrite
   its pages while every heap RID changes, so the match is by root bytes, not by RID (E2 R6).
2. **The root changed and every reachable node is in the file**: the value is read from the file, as after
   a whole-value UPDATE, which rewrites every fragment in place (E2 R8).
3. **The root changed and some nodes are absent**: the hybrid walk, as after a `.WRITE`, which leaves 736
   of 749 fragments of a 6 MB value out of the differential.
   - An absent node is looked up by node RID in the lake's maps.
   - A leaf takes its byte range from the old value's stored bytes, which come from the lake by design
     C (DQ-01, accepted; 089 D6 / D10): the lake value re-encoded, or the stored bytes `raw` where it
     does not re-encode exactly (022 D3).
   - An internal node is expanded through its old children.

   This is exact because a page absent from a differential is unchanged since the base (E1 4.5; all six
   E2 cases byte-exact).
4. **A node in neither.** In a loaded differential, when the node lies on a page the recovery marks stale
   (090): the row keeps its old bytes **and its map**, and the reader reports the page `stale` with reason
   `lob`; the next file resolves it (E6 rule 8: dropping the map lost 3 values for good). In any other
   file the reader refuses the value by name (089 D6, E1 rule 7) and the file is not applied; E1 met it
   only in the forced wrong-base apply that C4 refuses.

**The side input** `<run>/base/lob.parquet` (089 D10), read at S0, holds `(object_id, _mssql_rid,
column_id, root, nodes, text_exact, value, raw)` for every off-row value of every lake row the apply
deletes, i.e. the rows on pages of D: `value` the text and `raw` the stored bytes where design C keeps
them (DQ-01). That includes a REBUILD's old rows (their pages are freed), whose values the reader then
matches by root bytes. Its size follows the LOB rows on deleted pages: 19–125 values per
differential in E1, and the whole table after a REBUILD (which SQL Server also paid). Row-overflow
values (`varchar(n)` off-row) take the same path. "Reuse the lake's value by `_mssql_rid`" is never used:
it failed 10 of 21 E1 steps, silently, and the damage survives an incremental full.

### D7. Pending and stale pages

The decisions are 090's, inside the reader. The storage and the lifecycle are this spec's: the lake keeps
the sets, passes them back as side inputs, and records the outcomes.

**Pending pages** come from any file whose tail had transactions open at LastLSN: pre-undo images plus the
transaction ids (022 D11; E6 §6.2). At the next file, 090 applies the refined rule to each page P, in
this order (E6 §4):

1. **P is in the file**: P is processed normally and cleared.
2. **P is free** in the file's GAM or PFS (the lake's `alloc_pages` image stands in when that PFS page is
   absent, which means unchanged): the image is discarded, and P's rows go by the freed-page rule. This is
   the allocation check first. Without it, rolled-back transactions' rows come back: 97, 115 and 96 pages
   in E6.
3. **One of P's transactions is open** in the file's tail: P stays pending, and the lake keeps P's rows
   undone.
4. **Otherwise**, P's image is applied as if the file carried it, and cleared. What absence proves is
   "unchanged since the image", not "committed": one applied page in E6 belonged to a transaction that
   rolled back after splits had moved its rows away.

The applied images are joined *before* the file's own redo. Differentials do not reset the DCM, so a
differential's pending pages always resolve at the next file (E6 §3.3: 0 applied out of 2,341). The rule
matters after a full, and a later full resolves every pending page (D10).

**Stale pages** come from a loaded differential (E6 §3.4). Its tail changes pages the file does not carry,
and a lake of rows has no base image to redo them on.

- With `loaded_diff := 'skip'` (the default, owner variant 1), 090 skips whole every committed transaction
  with a record on such a page, and every later transaction on a page one of those left behind, to a fixed
  point. E6 skipped 127 transactions in D2 and 174 in D5. The lake gets an older but clean version: no
  duplicates and no torn LOB. E6 counted 383 and 518 stale keys, all healed by the next file.
- The pages touched are recorded in `stale_pages` from the reader's `changed_pages` rows of kind `stale`:
  the page, the reason (`baseless`, `skipped_transaction`, `lob`) and the skipped transaction ids.
- `'partial'` redoes what it can: 71 and 151 stale keys, but 27 and 79 rows shown twice until the next
  file.

**Healing.** A stale page's skipped record has an LSN above that file's FirstLSN, hence above W. So the
page's LSN in the next file exceeds W, and the page is in C. E6 carried 551 of 551 and 1,419 of 1,419
stale pages. The apply checks it, since the stale set is lake state and not a reader input (089 D10):
every page of `stale_pages` must be `changed` (above W) or `freed` in the next file. **A page that is
neither is a named error at that file (DQ-06, accepted): the import refuses the file, naming the page and
its table, and the way out is the next full with `mode := 'rebase'` (DQ-14).** A tail record older than
the previous file's LastLSN, on a page the file lacks, is already in the lake and is ignored. Rebuilding
such a page from its FORMAT_PAGE lost 3 rows for good (E6 rule 7, applied in 090 from `reflected_lsn`).

**Consistency, per import and per table**, from the reader's per-table status (090):

- `consistent`: `recovered`, and no pending page of the table remains;
- `pending`: `recovered`, with rows of transactions still open held back (pending pages remain);
- `stale`: committed work skipped until the next file (`stale`);
- a table the reader calls `unrecoverable` makes the file refused, naming it (090 D7).

It is reported in the summary, in `imports`, and in `ducklake_mssql_backup_history`. Before 090, a loaded
file is refused by default, as in 022 D11.

### D8. Partition maintenance (E7)

Incremental imports never change a table's partitioning or sort order, save 022 D5's one exception
(D5): when the backup's catalog removes a column a partition or sort field uses, by a DROP COLUMN or by
a type change done as ADD / DROP / RENAME, the table stays, so the importer removes only the field the
change hits; the rest of the spec, manual edits included, stays. A SQL Server re-create (DROP + CREATE
of a table) re-creates the lake table, and the new lake table gets the creation-time proposal; the old
table's manual spec edits go with the old table. After creation the layout is DuckLake's (owner,
2026-11-05, DQ-10). New rows land under the lake table's current spec, so a SQL Server
repartitioning only changes which DuckLake files the rewritten rows go to, and SQL Server's partition
numbers are never stored with the rows (E7§5). A spec set at creation stays valid after SPLIT / MERGE:
every row's value is computed from the row itself (after E7's s08 a fresh proposal would be
`epoch_year`, and the `epoch_month` spec set at creation keeps holding).

E7 replayed fifteen one-operation differentials with the rules below, exact in every one (E7§5):

| SQL Server operation | in the differential (089 § Partitioning) | in the lake |
| --- | --- | --- |
| SWITCH out, in, partition → partition | no data page; the two rowsets swap `idmajor` (and `numpart`) | step 5 of D3: the rows on the moved AUs' pages (the base IAM image) are copied lake-side into the target and deleted from the source by `_mssql_page` |
| SWITCH of a columnstore partition | no page; the columnstore and delete-bitmap rowsets swap (and the delta-store rowset, when the partition has one: unmeasured) | the compressed rows by rowset: those whose `02` rowgroup key carries the moved rowset id (088 D3); the delta-store and delete-bitmap rowsets move as AUs (`au_moved` with their base IAM pages), and the delta store's rows are found by `_mssql_page` in the `01` form |
| SWITCH, then DML on the switched rows | only the DML'd pages | those pages by the page rule, re-read under the new owner; the rest by step 5 |
| NEXT USED; SPLIT or MERGE of empty ranges | catalog pages only | mirror rows only |
| SPLIT, populated | the moved rows on new pages of a new rowset; the emptied pages freed | the freed pages' rows deleted by `_mssql_page`; new files, under the current spec, for the moved rows |
| MERGE, populated | the right partition's rows on new pages of the left rowset; its pages freed and its AUs gone | the same; a file left with no live row is dropped (D4) |
| TRUNCATE … WITH (PARTITIONS), small | pages freed; none in the diff | the `freed` pages' rows deleted by `_mssql_page` |
| TRUNCATE … WITH (PARTITIONS), big | the old AU type 0 / owner 0 (`dead_au`) with its pages still allocated; later `gone_au` and freed pages | the dead AU's pages, from the base IAM image, deleted by `_mssql_page`: the dead-AU clause of D3 step 4 (without it E7 kept all 9,000 rows) |
| REBUILD PARTITION | every row on new pages; new rowsets and in-row AUs; the LOB_DATA AU kept and re-owned | delete and re-insert; LOB values by root bytes (D6 step 1; 24 values in E7) |

**SWITCH, with DuckLake means (E7's ownership rule; owner, 2026-11-05).**

- A SWITCH writes no data, LOB or index page: the rowsets exchange `idmajor` (and `numpart`), the AUs stay
  with their rowsets, page headers and IAMs are unchanged, no DCM bit is set (E7, five variants). The page
  rule alone leaves the rows in the old table: E7 left 560 rows in `m_ci`, and after a SWITCH and a one-row
  UPDATE only the 37 rows of the touched page moved and 263 stayed.
- **The rule**: after the mirror update, every AU of the base mirror (columnstore: every rowset) is
  resolved through `sysallocunits.ownerid` → `sysrowsets`. When the (object, index) differs from the lake
  table its rows are in, they move; when only `numpart` differs, nothing happens (SPLIT / MERGE
  renumbering). The reader reports the moved AUs as `au_moved` (089): which AU moved from which table and
  partition to which.
- **The moved rows are found without a per-row AU**: they are the rows whose `_mssql_page` is one of the
  AU's pages in the base IAM image, the `au_moved` rows with `holds_rows` (columnstore: a compressed row
  by the moved rowset id in its `02` rowgroup key, 088 D3; the delta-store and delete-bitmap rowsets
  move as AUs, `au_moved` with their base IAM pages, and the delta store's rows are found by
  `_mssql_page` in the `01` form; the bitmap's pages hold no lake rows). The base IAM images record the
  AU, not the table, because a SWITCH changes the table and leaves the AU (022 D3).
- **The move is a lake-side copy plus a `DELETE`, never a file move.** In the import's transaction, before
  the new files (D3 step 5, D4):

  ```sql
  INSERT INTO lake.s.target SELECT … FROM lake.s.source
      WHERE _mssql_page IN (<the moved pages not in D>);  -- D's pages: deleted, re-read by the page rule
  DELETE FROM lake.s.source WHERE _mssql_page IN (<the moved pages>);
  ```

  DuckLake writes the target's rows in its current layout. Columns map by name. The moved rows keep
  their `_mssql_rid`, `_mssql_page` and `_mssql_lob`, because the pages did not change. The copy runs
  inside the import's transaction, and so inside the catalog's server transaction, so a big switched
  partition lengthens both (022 D1).
- **Not by partition boundary values.** A range predicate on the partitioning column, evaluated in
  DuckDB, would compare a string key by DuckDB's order, not the column's collation; the pages are exact
  for every key type.
- DuckLake could not move a file between tables anyway: a data file belongs to one table
  (`ducklake_data_file.table_id`), and expiry schedules a path for deletion without counting references
  (`DL/storage/ducklake_metadata_manager.cpp:5476-5530`), so a shared path would be deleted under the
  target.
- A SWITCH out and back within one differential's interval nets to nothing: only the final owner is
  compared (E7§5).
- **A SWITCH out to a table outside the import's filters** deletes the rows. A SWITCH in from such a table
  cannot be applied from a differential: the pages are unchanged, so they are absent. It is refused by
  name, with "apply the next full with `mode := 'rebase'`".

Not covered by E7, and fixture-gated before they are trusted (089 § Partitioning): RANGE LEFT SPLIT /
MERGE, an `ALL TO` scheme after a SPLIT, SWITCH … WITH TRUNCATE_TARGET, a partitioned CCI's SPLIT / MERGE
(E11), several operations in one differential.

### D9. Columnstore (with 088; E5)

A compressed row has no page. Its `_mssql_page` holds its rowgroup key, `02` + 16 hex digits of rowset
id + 8 of rowgroup (022 D3), and its `_mssql_rid` is the BLOB `0x02 ‖ u64 rowset_id ‖ u32 rowgroup ‖ u32
tuple` (088 D3). A delta-store row has the delta page as `_mssql_page`, in the `01` form, and
`0x03 ‖ u64 rowset_id ‖ u32 rowgroup ‖` its hidden key as `_mssql_rid`. The rowset id is in both
because rowgroup ids are numbered per partition and one lake table holds every partition. There is no
`_mssql_rowgroup` column (DQ-02).

- **Delete bitmap.** Its pages are rowstore pages (PAGE / CD records, E5 §3.7). For each bitmap page in
  C, its entries (g, t) delete, one `DELETE` per rowgroup, separate from the table's delete by page,
  `WHERE _mssql_page = <rowgroup key of g> AND _mssql_rid IN (<the entries' locators>)`; the rowgroup
  key, a constant, prunes the files (D4), and nothing prunes on the BLOB `_mssql_rid`. An entry for a
  tuple deleted before the export, or by an earlier file, matches no row, so the importer needs no
  bitmap history (D4, idempotent). Entries leave the bitmap only with their rowgroup.
- **New compressed rowgroups** (new `syscsrowgroups` rows in the mirror) are exported: their segments are
  newly written LOB pages, so they are in the file.
- **State transitions**:
  - the tuple mover compressing a delta store frees the delta's pages (U: its rows go) and adds a
    rowgroup (inserted);
  - a rowgroup that turned TOMBSTONE, was merged, or vanished in a REORGANIZE or REBUILD, is in A
    (`dead_rowgroup`, one of D's kinds, D4): its row's `page` is the rowgroup key in the `02` form, and
    its rows are deleted by `_mssql_page` = that key, like a page (a file left with no live row is
    dropped);
  - a TOMBSTONE's delta store can still own allocated pages, so the reader reports it as a dead rowset,
    not as data: `dead_rowgroup` rows carrying the delta-store AU's pages in its base IAM image, in the
    `01` form, whose rows go like a dead AU's.
- **A nonclustered columnstore index** is an index: the table's data is its rowstore, so nothing is
  imported from it.
- **Ordered CCI**: no sort is proposed for it at creation (022 D5, DQ-25); a sort a user sets stays.
- **A partitioned CCI's SWITCH** moves its compressed rows by rowset and its delta store as an AU (D8);
  a SWITCH of a partition with an OPEN delta store is unmeasured (a fixture, Testing). Its SPLIT / MERGE
  waits for E11.
- **Under load**: the tuple mover running during a backup is untested (E5 §6.6; 088's E11 measures
  it), and so are segment pages touched in a loaded tail. 090 D7 makes such a table `unrecoverable`,
  so the file is refused, naming the table (DQ-07, accepted: refuse the file until 090's columnstore
  extension and E11).

### D10. A later full, and rebase

- **Applied incrementally** (the default). The changed set is the full's pages with LSN > W: 227 of 1,244
  in E1's F1. The freed set is complete, because a full carries every allocated extent and every PFS page.
  Every pending page resolves, every stale page heals, and a non-copy-only full becomes the new base and
  starts a generation (C5, DQ-16). The result equals a fresh import of the same full (E1 and E6:
  identical lakes, `__mssql_sys` and locators included).
- **It repairs nothing.** It reads only changed pages, so earlier damage survives (E1 rule 10).
- **`mode := 'rebase'`** reads every page of the full whatever its LSN: every row of every imported table is
  deleted and re-inserted. That costs a fresh import's writes, in the same lake, with history kept. It is
  for:
  - after a recovery fork, while 085 cannot read the file's fork id, and to record the fork on a lake
    that has none (C1, 089 D7);
  - after a change of the import filters (022 D1);
  - for a SWITCH in from outside the filters (D8);
  - for repair.

  The plan shows its size first.

### D11. The commit for a file

**Statements and calls** (022 D7, plus), all in the expansion, after the validation and before the
apply:

- the DDL of D5;
- the per-table `DELETE`s of D4 and a SWITCH's `INSERT … SELECT` and `DELETE` (D8): DuckLake's own
  operators, which make the delete files and the copy's data files themselves.

**What DuckLake writes**, on top of 022's list:

- a `ducklake_delete_file` row per rewritten delete file, and the end of its predecessor;
- `ducklake_files_scheduled_for_deletion` for superseded delete files and dropped data files;
- `end_snapshot` on dropped data files;
- per touched table, the stats refresh (spec 014's CTE `UPDATE`, in 1,000-row chunks);
- the ALTERs' column rows.

The commit takes the client loop: the server-side apply covers data-files-only commits, and this one has
deletes (`MD/mssql_server_commit.cpp:258-262`).

**Volume.** Rows ≈ F·(1 + C) + Fd·3 + Fx·2 + Tt·(1 + C) + DDL + P·8, where Fd = delete files rewritten,
Fx = files dropped, Tt = tables touched, and P = mirror and side files as in 022 D7. A typical
differential has 50 changed tables, 2 new files and 3 delete files each, and 10 mirrors changed: about
8 k rows. That is a few `mssql_exec` round trips and the appender's bulk loads, under a second to a few
seconds (estimate). A hot data file's cumulative delete file is rewritten at every import that touches
it, so its size grows with the rows deleted. DuckLake's `ducklake_rewrite_data_files` rewrites files past
its delete threshold (022 D6, 024 §D5).

**Conflicts, retried, bounded.** DuckLake checks them at commit
(`DL/storage/ducklake_transaction_state.cpp`):

- a `DELETE` conflicts with a delete from, or a compaction of, the same file committed meanwhile
  (`:2479-2569`; a merge that removed the file, `:2540-2551`), and with any concurrent insert into the
  table, or an ALTER or DROP of it (`:295-305`);
- an append-only table, one this import only inserts into, does not conflict with a merge or a rewrite:
  an insert conflicts only with a drop, an ALTER or a delete (`:281-294`), and a compaction only with a
  drop (`:329-334`).

DuckLake's commit loop retries any attempt that fails after the conflict check with a retryable error
(`can_retry`, `:2665-2707`; `RetryOnError`, `DL/storage/ducklake_transaction.cpp:1395-1409`); a
conflict the check finds never is. The importer retries the conflict itself, in autocommit:
`ducklake_mssql_backup_commit` runs the post-export part (validation, DDL, `DELETE`s, a SWITCH's copy,
apply, or phase A's `ducklake_add_data_files`) as one transaction on a side connection, under 022
D12's `max_memory` (its `max_threads` has no effect there: no per-connection thread knob, 022 D12),
and on a conflict runs it again on the same export, up to N = `import.conflict_retries` times
(default 3), N+1 attempts in all, then reports (022 D1). Each retry repeats the `DELETE` scans and the
copy. Inside an explicit transaction the COMMIT fails, and running the PRAGMA again resumes from the
export.

- A compaction committed before the import's transaction changes nothing: the deletes find rows by value
  wherever they are, and the validation lets it through (022 D2.3). One committed during it conflicts
  only when it merged or rewrote a file the import deletes from; the retry's validation then passes it.
- A user writing to an imported table meanwhile makes the retry's validation refuse (D12). Nothing is
  lost: the run is resumable.
- The likeliest collision is a compaction or `CHECKPOINT` of an imported table during an import: it
  merges the small files earlier differentials left, which are the next import's delete targets. The
  docs recommend running maintenance between imports.

### D12. Writes by others to imported tables

The importer is the only writer of imported tables, `__mssql_sys` and `__mssql_backup`. A user insert adds
rows that no page of the database holds, so no delete by page removes them: they would survive forever. A
user update or delete makes the lake differ from the database until that page next changes, when the
delete by page and the page's new rows overwrite it.

So the callback lists the snapshots since L from `ducklake_snapshots` (`changes_made`), excluding the
importer's own, and checks what they touched:

- `inserted_into`, `deleted_from`, inlined inserts or deletes, or an ALTER of an imported table other than
  partitioning, sort, options, comments or a rename: refused with `foreign_writes := 'error'`, naming the
  snapshot, its author and the table; `'ignore'` accepts the divergence and records it;
- a compaction (`ducklake_merge_adjacent_files`, `ducklake_rewrite_data_files`, a `CHECKPOINT`'s): allowed;
  it moves rows between files and changes no row;
- a manual DROP of an imported table: refused, with "re-create it, or exclude it from the filters and
  rebase".

## Enforcement & security

- **Fail closed** on every chain rule (D2; a fork id the file does not yield or the lake did not record
  refuses every apply by LSN, C1 by 089 D7's rule), on a plan-id mismatch, on a foreign write, on the
  attribution check's refusal of a page the allocation images do not attribute to its header AU
  (`mssql_backup_allocation`, repeated by the export; D4: the file is refused, naming it), on a stale
  page the next file neither carries nor frees (a named error, DQ-06), and on an unresolvable LOB
  (refused by name; only on a page a loaded differential's recovery marks stale is the old value kept
  with its map and the page reported `stale`, D6 step 4). Nothing is guessed: a refusal names the file,
  the GUIDs or LSNs, the table and the snapshot.
- **No dirt.** Rows of open transactions never reach a user table (090, D7). The pending images do live in
  `__mssql_sys.pending_pages` and in its history until expiry (DQ-09).
- **Side inputs and run files** (`base/catalog/`, `base/allocation.parquet`, `base/pending.parquet`,
  `base/lob.parquet`, `delete_pages.parquet`, the moved-page lists) contain database data. They are
  written in the run directory, outside DATA_PATH (`run_path`, 022 D9), which needs the protection the
  lake's data has, and deleted after the commit.
- **Trust in the reader.** Every decision about pages comes from the file plus the lake's own state, never
  from user input.
- **Version mismatches.** A DuckLake bump re-runs the ordering check of D4 (the deletes before the new
  files, one snapshot). A reader version change between imports is allowed. The imports record it, and a
  mirror schema change is applied as D5's last row.

## Testing

**The oracle** is 022's: at every snapshot, RESTORE of the same chain prefix against `AT (VERSION => n)`,
per table, multiset over canonical rows, plus locators. Every chain also runs twice, through phase A
(`ducklake_add_data_files`) and phase B (`AppendFiles`), and the two lakes must agree row for row.

**Quiet chains (E1)**, all exact at every step, with the lake-against-lake identities checked
(`__mssql_sys` and locators included):

| chain | files | identity |
| --- | --- | --- |
| A | F0 → D1 → D2 → F1 (incremental) → D3 → D4 → D5 → F2 (incremental) → D6 → D7 | after F1 = F1 fresh; after F2 = F2 fresh |
| A' | F0 → D1 → D1b (no-change differential) | — |
| B | F0 → D2 (D1 skipped) → F1 → D3 → D4 → F2 → D6 | after D4 = C; after D6 = D |
| C | F1 fresh → D4 (D3 skipped) | — |
| D | F2 fresh → D6 | = A after D6 |

The refusals are checked too:

- D3 onto F0 + D1 + D2, the wrong base;
- D1 or F0 onto the same lake, out of order;
- a file from another database;
- a differential, or a full applied incrementally, while the file's recovery fork cannot be read (C1
  case a): refused, naming the reason, while a full with `mode := 'rebase'` is accepted; on a lake with
  no fork recorded once it can be read (case b): refused, then a rebase on a full records the fork and
  the next differential applies;
- a file already applied, which is skipped.

**Allocation edges (E2):**

- DROP (small, and big deferred), TRUNCATE (small, and big deferred), a TABLOCK delete, and ghost cleanup,
  each taken in the same batch (D1) and after cleanup (D2);
- SHRINKFILE, which leaves pages past `Size`;
- heap and clustered REBUILD, with LOBs reused by root;
- the ALTER sequence: ADD with defaults, DROP, int → bigint, varchar widening, REBUILD;
- the six `.WRITE` cases;
- a second data file, a filegroup, a striped differential;
- FULL, SIMPLE and BULK_LOGGED with minimally logged operations.

**Schema evolution**: one scenario per row of D5's table, including:

- drop and re-create under the same name after a manual spec change: the new lake table gets the
  creation-time proposal, and the old table's manual edits go with it;
- TRANSFER;
- a rename after a manual lake rename (the manual name wins);
- a metadata-only type change that DuckLake cannot express;
- a DROP COLUMN, and a type change by ADD / DROP / RENAME, of a partition column and of a sort column,
  under a spec with manual edits: that field alone is removed, the rest kept, and the WARNING, the
  commit message and `imports` name it;
- a decimal promotion past 18 digits under a `bucket` field: DuckLake starts a new partition spec, and
  the older files keep their buckets.

**Load (E6, with 090)**:

| cycle | files | expected after each |
| --- | --- | --- |
| 1 | F (T1 commits later, T2 rolls back, T3 commits after touching other pages, T5 commits during the backup, LOB updates) → D1 quiet → D2 loaded (T4, T6 open) → D3 quiet | exact, exact, **0 dirty** with only stale keys (skip: E6 383, all stale), exact |
| 2 | F2 loaded, applied incrementally → D4 quiet | exact, exact; = F2 imported fresh, then D4 |
| 3 | F3 with T7 / T8 open across F3 **and** D5 → D5 loaded → D6 quiet (T7 commits, T8 rolls back) | exact; 0 dirty + stale (skip: 518); exact; = F3 fresh → D5 → D6 |

The pending outcomes are asserted against E6 §3.3: in file, applied, discarded, kept. The stale pages must
all heal at the next file.

**Columnstore (088)**:

- deletes and updates (bitmap plus delta);
- the tuple mover compressing a delta;
- TOMBSTONE, REORGANIZE, REBUILD;
- a partitioned CCI;
- a SWITCH of a partition that has an OPEN delta store (unmeasured: E7's s12 showed only the columnstore
  and delete-bitmap rowsets swap): the delta store's rows move by its AU's base IAM pages, the
  compressed rows by rowgroup key;
- the same bitmap page carried by two files (idempotence).

**Partition maintenance (E7)**: E7's maintenance database and its fifteen one-operation chains
(`experiments/E7/sql/b00`–`b02`, `s01`–`s14`: SWITCH out, in, partition → partition, columnstore, SWITCH
followed by DML and by a one-row UPDATE, NEXT USED, SPLIT and MERGE empty and populated, TRUNCATE
PARTITIONS small, deferred and 30 s later, REBUILD PARTITION), each table compared with RESTORE; plus
staging tables outside the filters (SWITCH out deletes, SWITCH in is refused naming rebase), RANGE LEFT
SPLIT / MERGE, and a SWITCH out and back inside one differential's interval (nothing moves). The lake
tables keep the spec they were created with, whatever SQL Server does.

**Writes by others**: a user INSERT, DELETE or ALTER on an imported table makes the next import refuse. A
`CHECKPOINT`, `ducklake_merge_adjacent_files` or `ducklake_rewrite_data_files` on imported tables between
two imports changes nothing (the oracle reports 0). Committed while an import's transaction runs (D11):

- one that merges or rewrites a file the import deletes from: the attempt fails by DuckLake's conflict
  check and the retry commits on the same export (no unit written twice), and the lake equals RESTORE;
- conflicting compactions on every one of the N = `import.conflict_retries` retries (N+1 attempts):
  the import reports the conflict and the attempts, and a later run resumes;
- the same conflict in phase A: `ducklake_add_data_files`' registration is retried the same way;
- one that rewrites no such file, or touches only an append-only table of the import: it commits first
  try;
- inside an explicit transaction, the COMMIT fails with DuckLake's conflict, and running the PRAGMA again
  resumes from the export.

**Counterfactuals.** Each rule is switched off alone (test-only switches, in mssql for the reader's rules
and in mssql-ducklake for the lake's), and the expected failure is the measured one:

| rule switched off | expected | source |
| --- | --- | --- |
| frees from the PFS (changed pages only) | `t_trunc` keeps its pre-TRUNCATE rows: 850 against 50 | E1 4.2 |
| R3 liveness (`Size` + GAM + PFS) on row sources | after SHRINKFILE, 4,000 rows against 2,000 | E2 §2 |
| delete by page in the table the base IAM image names (in the page's new owner instead) | row 801 of a truncated table survives on a page reused by `t_pk` | E1 4.3 |
| `sysseobjvalues` in the mirror | ADD COLUMN … DEFAULT reads NULL: 6,450 rows (D2), 8,787 (D3) | E1 4.8 |
| the chain check (`--force` the wrong base) | 3 tables corrupted (181 rows, 1 row, 40 LOBs) | E1 §3 |
| the fragment map (reuse by `_mssql_rid`) | 10 of 21 steps wrong, persisting through an incremental full | E1 4.5 |
| keep an unresolved LOB's map (drop it) | 3 LOBs lost for good | E6 §3.2 |
| the pending-page rule | 2,282 committed keys lost at D1, never healed (2,187 at D3) | E6 §3.2 |
| the allocation check before applying an image | rolled-back rows resurrected when the interval's PFS page is absent (a constructed case: E6 had the PFS page by luck) | E6 §4 |
| images applied before the file's redo | a tail record on P misses its base (constructed) | E6 §4 |
| CheckpointLSN order (FirstLSN instead) | a valid D5 is refused | E6 §3.4 |
| ignore tail records older than the previous LastLSN | 3 rows lost for good in skip mode | E6 §3.4 |
| skip whole (partial instead) | interim duplicates and torn LOBs (27 rows twice and 12 LOBs at D2), healed later | E6 §3.2 |
| no recovery at all (before 090) | 8,457 differing keys, 7,516 of them dirty | E6 §3.2 |
| the deletes before the new files (registered after them instead) | every changed page's new rows deleted with its old ones | D4 |
| the ownership rule (D3 step 5) | the five SWITCH variants fail: rows stay in the old table (560 in `m_ci`; after SWITCH + one UPDATE, 263) | E7 §5 |
| the dead-AU clause (A in D3 step 4) | a deferred `TRUNCATE … WITH (PARTITIONS)` keeps all 9,000 rows | E7 §5 |

**Volume and time**: one E1-sized and one 2,000-table differential against the SQL Server catalog; the
statements, rows and seconds recorded.

## Alternatives considered

- **Replace each changed table whole** (research-architecture §2): exact and simple, but every import
  rewrites whole tables, and `ducklake_table_changes` shows them replaced.
- **A PK-keyed delta**: many tables have no key (RESEARCH §5). The physical locators and the page are the
  key that every table has.
- **MERGE**: DuckLake allows one UPDATE/DELETE action. DELETE + INSERT in one transaction is the same single
  snapshot.
- **Deleting by page in every table** (E1's way): exact, but every delete reads every table's files, so
  its cost follows the lake's size. The base IAM image names the one table.
- **Keeping raw record bytes per row** so that a loaded differential is exact on its own (the E6 oracle run
  shows it would be): it roughly doubles storage, and the owner chose variant 1.
- **A metadata-only SWITCH, or moving the source's whole files to the target**: DuckLake has no file move
  and no reference-counted cleanup (D8), and the owner chose DuckLake means: a lake-side copy plus a
  `DELETE` by `_mssql_page` (2026-11-05, DQ-10).
- **A SWITCH by partition boundary values** (`INSERT … WHERE key >= b1 AND key < b2`): evaluated in DuckDB,
  it compares a string key by DuckDB's order, not the column's collation. The moved pages are exact for
  every key type (D8).

## Decisions

Each is decided or accepted, in the list merged with the mssql series:
[decisions.md](https://github.com/hugr-lab/mssql-extension/blob/main/specs/085-backup-reader-core/decisions.md).

| # | question | decision | DECISIONS |
| --- | --- | --- | --- |
| 1 | a lake born from a copy-only full (C6) | the base its header reports, once verified on the E1 corpus; else differentials refused until a non-copy-only full | DQ-15 |
| 2 | renames on both sides | a SQL Server rename applied only while the lake name is the importer's (D5) | DQ-27 |
| 3 | offer `loaded_diff := 'partial'` at all | yes, as a non-default, documented option (D1) | DQ-17 |
| 4 | columnstore under load | refuse the file until 090's columnstore extension and E11 (D9) | DQ-07 |
| 5 | writes by others | refused by default (D12) | DQ-08 |
| 6 | `mode := 'rebase'` in v1 | yes (D10) | DQ-14 |
| 7 | a stale page the next file neither carries nor frees | a named error; the way out is a rebase (D7) | DQ-06 |
| 8 | which fulls start a generation | non-copy-only fulls only (C5, 024 D1) | DQ-16 |

## Follow-ups

- **A verify mode**: a fresh import of a full into a scratch lake, compared with the lake's state at the same
  backup, without writing (E1 rule 10's insurance), reporting the differing keys per table.
- **Row-level change minimisation**: today every row of a changed page is deleted and re-inserted, so
  `ducklake_table_changes` shows unchanged rows as delete + insert. Keeping unchanged rows (comparing by
  `_mssql_rid` and content) would make the change feed exact. It needs the old rows read back.
- **Log backups** (RESEARCH phase 4): the same page apply, driven by redo of a log chain.
- **External workers for differentials** (022 D4 already allows it; the side inputs must reach the
  workers).

## Tasks

| # | task | estimate |
| --- | --- | ---: |
| 1 | Chain validation C1–C7 in the callback and the apply; `if_applied`; the fork check, failing closed by 089 D7's rule (both cases); the refusal messages | 3 d |
| 2 | The changed, unallocated and dead sets from `mssql_backup_changed_pages(…, expand := true)`: D4's kinds, data pages only, the import filters on either owner; each page's lake table from its base owner (`from_object_id`) through the registry; the per-table `delete_pages.parquet` | 3 d |
| 3 | The `base/` side inputs (catalog, allocation, pending, LOB); the `alloc_pages` replacement (D3 step 7); the mirror apply; the catalog diff against the registry → the DDL decision list | 5 d |
| 4 | Schema evolution: native ALTERs (D5), the manual-wins checks, removing only a dropped or retyped column's partition or sort field (recorded, with a WARNING), lake-side exports for TRANSFER and inexpressible changes | 6 d |
| 5 | Deletes: the per-table `DELETE … WHERE _mssql_page IN (…)` statements in the expansion, before the new files; the test that `DELETE` + `ducklake_add_data_files` (confirmed by mssql-ducklake, 2026-11-06) and `AppendFiles` in one transaction make one snapshot and leave the new rows alone | 2 d |
| 6 | Delete cost: D4's open check (an `IN (SELECT …)` delete prunes the VARCHAR `_mssql_page` through DuckDB's dynamic join filter, as measured for BIGINT, else the delete set as constants, chunked `IN` lists with `BETWEEN` bounds), and with 022 task 11 the test that keeps the answered one (the hex orders as bytes under the catalog's BIN2 UTF-8 statistics columns); the files a one-page delete reads, measured on a heap, a clustered table after splits and a sorted table, after N differentials and after a `ducklake_merge_adjacent_files` | 2 d |
| 7 | LOBs: `base/lob.parquet`, reuse by root, REBUILD by root bytes, an unresolvable value kept with its map in a loaded file | 5 d |
| 8 | Pending and stale tables and their lifecycle with 090: the outcomes, the skip default, the heal assertion, consistency reporting | 6 d |
| 9 | Partition maintenance (E7): SPLIT, MERGE, TRUNCATE, REBUILD PARTITION by the general rules; SWITCH by the ownership rule (`au_moved` → the AU's pages in the base IAM image → `INSERT … SELECT` into the target + `DELETE` by `_mssql_page` in the source); E7's fifteen chains and the two counterfactuals | 5 d |
| 10 | Columnstore: bitmap entries → `DELETE` by rowgroup key and `_mssql_rid`, rowgroup transitions by rowgroup key, dead rowsets, a SWITCH's delta store as an AU (after 088) | 4 d |
| 11 | Writes by others: detection from `ducklake_snapshots`, refusal, compactions let through; the conflict retry's tests (D11: a compaction during the commit, retries exhausted, an append-only table, an explicit transaction) | 2.5 d |
| 12 | Later fulls and `rebase` | 2 d |
| 13 | The oracle runs: E1 chains, E2 edges, E6 cycles, schema-evolution and partition scenarios, the counterfactual switches and their assertions, phase A against phase B | 9 d |
| 14 | Docs page and examples | 2 d |
| | **total** | **56.5 d ≈ 11.5 wk** |

By phase (series.md):

- **I3**, needing 089: tasks 1–7, 9 (rowstore SWITCH included, now that E7 is done), 11–12, 14, and
  13 but its E6 cycles (6 of its 9 d): 43.5 d, about 8.5 weeks.
- **I4**, needing 090: task 8 and 13's E6 cycles (3 d): 9 d, about 2 weeks.
- **I5**, needing 088: task 10 and the columnstore SWITCH by rowset: 4 d, about 1 week.

series.md's 10–11.5 weeks allows for overlap.
