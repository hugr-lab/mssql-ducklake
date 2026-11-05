# Spec 022: applying a differential or a later full on top of the lake

- **Status**: draft (number tentative)
- **Date**: 2026-11-05
- **Author**: vgsml, with Claude

## Summary

After 021's first import, the lake is the base. Each later file is applied with nothing but the lake and
that one file, never the old full and never an intermediate differential. A file can be:

- a differential;
- a periodic new full;
- later, a log backup.

Each applied file is one snapshot, registered by the same plan → export → register pipeline as 021. What
this spec adds is the apply algorithm, which E1 proved on 21 states over 5 chains and E6 made exact under
load:

1. **Chain checks.** A file is validated against the lake's own provenance first.
2. **The catalog first.** The `__mssql_sys` mirror is updated, and its diff becomes DuckLake DDL inside
   the same commit.
3. **Rows by page.** The rows of every changed, freed or dead page are deleted **in every table**, as
   positional deletes computed from the page map with no data scan. Then the pages' new rows are
   inserted.
4. **LOBs.** A LOB only partly in the file is rebuilt from the lake's fragment map.
5. **Pending and stale pages** carry what a loaded file could not settle to the next file, which heals
   it.
6. **The rest.** Partition maintenance (E7's rules; a SWITCH by lake-side copy) and columnstore deletes are
   handled in the same commit.

Owner questions are cited as DQ-nn ([decisions.md](https://github.com/hugr-lab/mssql-extension/blob/main/specs/085-backup-reader-core/decisions.md)). The layout of every file the apply writes is
DuckLake's (owner, 2026-11-05; 021 D5, DQ-10).

## Problem

DuckLake has no notion of a page. A differential carries only the extents changed since its base full
(E2 R1: DCM ∩ GAM-allocated). It does not carry the evidence of a delete, only the allocation pages that
show it (E2 R2). And it rewrites whole pages whose rows are mostly unchanged: in E1, only 3–68 % of a
differential's pages had changed at all. Getting the lake to equal the database at each backup took
twelve rules, each shown necessary by a failure:

- the PFS for frees;
- delete by page in every table;
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

- A native `DELETE … WHERE _mssql_page IN (…)` reads `_mssql_page` of every file of every table, so its
  cost grows with the lake, not with the change.
- DuckLake's `MERGE` allows one UPDATE/DELETE action (`DL/storage/ducklake_merge_into.cpp:11-23`).
- A transaction per table breaks the one-snapshot rule.

Paths as in 021: `DL/` = `ducklake/src/`, `DD/` = `duckdb/`, `MD/` = `src/`.

## Design

### D1. The surface

The same PRAGMA as 021 decides from the header which kind of file it holds. A strict form refuses
anything but a differential:

```sql
PRAGMA ducklake_import_mssql_backup('lake', 's3://bak/sales_full_2026-12-01.bak');      -- a later full
PRAGMA ducklake_import_mssql_diff_backup('lake', 's3://bak/sales_diff_2026-11-05.bak');  -- a differential only
```

These options are added to 021's:

| option | default | meaning |
| --- | --- | --- |
| `mode` | `'auto'` | `'incremental'` applies the changed pages (D3); `'rebase'` (fulls only) reads every page of the full whatever its LSN (D10) |
| `loaded_diff` | `config`, else `'skip'` | for a loaded differential (with 090), passed to the reader's parameter of the same name (090 D4): `'skip'` skips whole every tail transaction that touches a page the file lacks (the owner's variant 1); `'partial'` redoes what it can (fewer stale keys, but interim duplicates and torn LOBs, E6 §3.4); whether to offer `'partial'` at all is DQ-17 |
| `foreign_writes` | `'error'` | what to do when a table was written by a non-import snapshot since the last import (D12) |
| `partitioning`, `sorting` | derived | as in 021, for tables this file creates |

### D2. Chain validation and ordering

The rules below run in the callback at S0, and again in the apply at S1. L is the last applied import
(from `__mssql_backup.imports`) and B is the lake's diff base. The reader's `mssql_backup_chain_check`
(089) evaluates C1, C3, C4 and C5 from the file alone; the importer adds C2 (every import, not only the
last), C6 and C7.

| # | rule | refusal names | source |
| --- | --- | --- | --- |
| C1 | same database: FamilyGUID equal (085's `family_guid`, `dbi_familyGUID`), and RecoveryForkID equal to the lake's current fork (085 `recovery_fork_id`: to locate; until it is, the fork is not checked and the import says so). FamilyGUID survives a restore, so a restored copy elsewhere passes it; the fork id is what tells the two lines apart | both GUIDs; "the database was restored or forked: apply a full with `mode := 'rebase'`" | RESEARCH §3 (FamilyGUID); after a fork the LSNs restart below the lake's, so the LSN filter would be wrong |
| C2 | not yet applied: BackupSetGUID ∉ `imports` | `if_applied := 'skip'` returns that snapshot | 021 D9 |
| C3 | order by **CheckpointLSN**: f.CheckpointLSN > L.CheckpointLSN | both LSNs and L's snapshot | E6 §3.4: a transaction open across two files gives both the same FirstLSN, and E1's FirstLSN check refused a valid D5 |
| C4 | a differential's base: f.DifferentialBaseGUID = B.guid and f.DifferentialBaseLSN = B.lsn | "differential base 63da… is not the lake's base c53c…; apply full 63da… first" when known | E1 rule 1; forcing D3 onto a lake that skipped F1 corrupted 3 tables |
| C5 | a full is accepted after C1–C3; a **non-copy-only** full becomes the new base (B := f); a copy-only full changes neither the base nor the DCM. Which fulls start a generation is DQ-16 (023 D1 drafts: any full) | — | RESEARCH §6.2, E6 Q1 |
| C6 | a lake whose first import was a copy-only full: B := the base that file's header reports, if it reports the database's current diff base (**to verify** on the E1 corpus); otherwise B is unknown and differentials are refused until a non-copy-only full is applied | "no differential base yet" | DQ-15 |
| C7 | a striped set is complete (FamilyCount files, one MediaSetId; the reader refuses otherwise) and the family is recorded | — | E2 R9 |

What the reader is given (089, 090):

- `since_lsn := W`, with W = min(L.FirstLSN, L.CheckpointLSN) (E1 rule 2, E6);
- `reflected_lsn := L.LastLSN` (E6 rule 7);
- `base := '<run>/base/'` (089 D10), written by the callback from the lake at S0:
  `catalog/<base_table>.parquet` (the `__mssql_sys` mirrors), `allocation.parquet` (`alloc_pages`),
  `pending.parquet` (`pending_pages`) and `lob.parquet` (D6);
- `recovery := true` and `loaded_diff` (the option of D1; 090 D4).

The stale page set is not given to the reader: it stays in `stale_pages`, and the apply checks its healing
against the reader's output (D7).

A differential needs no intermediate file: differentials are cumulative since their base, so a later one
can follow an earlier one directly, as D2 followed F0 with D1 skipped (E1 chain B).

### D3. The apply, in order, in one transaction

The callback plans at S0 through a side connection, as in 021. A differential also needs the lake's
mirror as its catalog: a diff does not carry the unchanged catalog pages. So the callback:

1. writes the base side files of D2 except `lob.parquet` into `<run>/base/`, from `__mssql_sys` at S0;
2. asks the reader for the file's catalog over them (`mssql_backup_tables(file, base := …)`, 089) and diffs
   it against the registry by object id and column id (D5);
3. asks for the delete and move evidence, `mssql_backup_changed_pages(file, since_lsn := W, base := …,
   reflected_lsn := …, recovery := true, loaded_diff := …)`, joins it with `page_map` at S0, and writes the
   delete positions (D4), the ownership moves (D8) and `base/lob.parquet` for the rows the apply will delete
   (D6);
4. writes `plan.json`.

The expansion:

```sql
PRAGMA ducklake_import_mssql_diff_backup('lake', 's3://bak/sales_diff_2026-11-05.bak');
-- expands to (the side inputs are already in the run directory, read AT (VERSION => S0)):
CREATE TEMP TABLE __mssql_backup_manifest_<run> AS
    SELECT * FROM ducklake_mssql_backup_export('lake', '…', plan := '<run>/plan.json');
ALTER TABLE lake.dbo.t_pk ADD COLUMN c_def INTEGER DEFAULT 42;       -- the DDL of D5, in order
CREATE TABLE lake.dbo.t_new (…) WITH (data_inlining_row_limit = 0, auto_compact = false, …);
DROP TABLE lake.dbo.t_drop;
SELECT * FROM ducklake_mssql_backup_apply('lake', '…', manifest := …, plan := …);
```

The algorithm (the numbered steps are the order of effect; E1 §2, E6 §3.1, E7§5):

0. **Page-map health.** Page-map rows that name files no longer live are stale after any compaction, the
   importer's or a foreign one. The callback rebuilds runs for each live file of an imported table that
   has no page-map rows, from `filename, file_row_number, _mssql_page` at S0 (021 D6). The apply deletes
   the stale rows and appends the rebuilt ones.
1. **Pending pages first** (with 090; D7): the reader resolves each pending page against the file *before*
   its own redo, so a tail record on P finds its base. Applied images join the file's page store as if the
   file carried them.
2. **Export**: the rows of every page in the changed set C are written as new data files with their runs,
   in the lake table's current DuckLake layout (spec, sort order, file size; 021 D5). C is:
   - pages with LSN > W;
   - applied pending images;
   - pages formatted in the tail (E6 §3.1).

   The rows come only from PFS-allocated data pages whose header names a known in-row allocation unit,
   skipping ghost and stub records (E1 rule 4, E2 R3/R4); a row's table is its AU's owner in the file's
   catalog. The same holds for the mirrors' catalog pages. The same phase writes the lake-side copies of
   the moved rows (step 5). The reader also reports, through `mssql_backup_changed_pages`,
   `mssql_backup_pending_pages` and `mssql_backup_tables`:
   - the unallocated ranges U (`freed`): per interval from the file's PFS / GAM, else the lake's
     allocation image, plus every page past the file's `Size`;
   - the dead allocation units and rowsets A: `dead_au` (type 0 / owner 0), `gone_au`, and for a
     columnstore `dead_rowgroup` (a TOMBSTONE or vanished rowgroup, 088) (E2 R5, E5 §3.7, E7 s10);
   - the moved allocation units M (`au_moved`, E7);
   - the new pending and stale sets;
   - the skipped transactions, and the status per table.
3. **Catalog first** (E1 rule 6): the DDL statements above. The mirror tables are applied exactly like
   user tables in steps 4–6, because their rows live on pages too.
4. **Delete by page, in every table** (E1 rule 5): the delete set is D = C ∪ (page_map pages in U) ∪
   (page_map rows whose `au_id` ∈ A) ∪ the pages of dropped tables. One lookup in the global `page_map`
   finds every file holding rows from those pages, whichever table owns the file now. A page that moved
   from a truncated table to another (E1 4.3) is found the same way. Its positions become delete files
   (D4).
5. **Ownership** (E7's rule, D8): every lake row whose `page_map.au_id` is in M, and whose page is not in
   D (those were just re-read under their new owner), moves to the owner the file's catalog gives: its
   lake-side copy in the target (written in step 2) is registered, and its positions in the source become
   delete files. A change of `numpart` alone moves nothing.
6. **Insert**: `AppendFiles` for the new files.
7. **Page map**: delete the rows of D, the moved rows' old runs and the stale rows of step 0; append the
   new runs (a moved row keeps its page and AU, with a new file and position).
8. **Side tables**:
   - `pending_pages` := (old − resolved) + new;
   - `stale_pages` := (old − healed) + new;
   - `alloc_pages`: changed allocation pages replaced.
9. **Provenance and registry**: the `imports` row; `tables`, `columns` and `views` updated; the commit
   message (021 D8).

Page splits, moves, forwarding, rebuilds, shrinks, SPLIT / MERGE / TRUNCATE of partitions and pages
reused across tables need nothing special: every page involved is in C, U or A, so its old rows go and its
new rows come (E1 rule 5, E2 §2, E7§5). Only a SWITCH needs step 5. Re-processing a page is idempotent
(E1 rule 2).

### D4. Positional deletes from the page map

**The positions are computed in the plan, at S0.** D is known from the file's page index and the mirror
diff before any row is decoded, through `mssql_backup_changed_pages(file, since_lsn := W, base := …)`
(D3); the moved rows' source positions (D8) are computed the same way.

1. The callback joins D with `__mssql_sys.page_map AT (VERSION => S0)`. The page map is partitioned by
   `page_range` and sorted by `page`, so the lookup prunes.
2. The result is grouped by `file_path`, giving each data file's new delete positions
   `first_pos .. first_pos + row_count − 1`.
3. The positions are written to `<run>/deletes.parquet` (an importer-private file, not a reader input).
4. The apply re-validates S1 against S0 (021 D2.3). A compaction of an affected file between S0 and S1
   is caught by the plan id, and failing that by DuckLake's own conflict check ("delete from file …
   compacted it", `DL/storage/ducklake_transaction_state.cpp:2480-2560`). The run is then planned again.

**Writing them, in the apply.** Per affected data file f, holding N new positions, the apply follows
`DuckLakeDelete::FlushDeleteWithSnapshots` (`DL/storage/ducklake_delete.cpp:422-478`), driven from
mssql-ducklake:

1. Read f's current deletes with `DuckLakeDeleteFilter::ScanDeleteFile`
   (`DL/include/storage/ducklake_delete_filter.hpp:51`). Old positions keep their snapshot ids, falling
   back to the delete file's begin snapshot.
2. Merge in N, dated `S1 + 1`, into a `set<PositionWithSnapshot>` keyed by position
   (`DL/include/storage/ducklake_delete.hpp:24-52`). A position deleted before stays deleted with its old
   snapshot, so the merge is idempotent; columnstore re-applications depend on that (D9).
3. If every row of f is gone, `transaction.DropFile(table, f.id, path, row_count, size)`
   (`DL/storage/ducklake_delete.cpp:408-420`).
4. Otherwise, `DuckLakeDeleteFileWriter::WriteDeleteFileWithSnapshots`, with
   `overwrites_existing_delete = true`, `overwritten_delete_file = {old id, old path}` and `max_snapshot`.
   The new file is cumulative: the commit drops the superseded row and schedules the old file for deletion
   (`DL/storage/ducklake_metadata_manager.cpp:4581-4603`). It also rewrites the file if a retry moved the
   commit snapshot (`DuckLakeDeleteFileWriter::SetCommitSnapshot`, `ducklake_delete.hpp:88-90`).
5. `transaction.AddDeletes(table, files)` per table.

The cost is O(changed rows) for the lookup, plus O(the deletes already in each touched file) for the
cumulative rewrite. Puffin deletion vectors stay off: they are experimental
(`DL/functions/ducklake_options.cpp:40`).

**The native fallback and the oracle.**

- Per table that the page map says has rows on D (or every table, E1's way), and before the inserts:
  `DELETE FROM lake.s.t WHERE _mssql_page IN (SELECT page FROM read_parquet('<run>/delete_pages.parquet'))`.
- This is DuckLake's own delete operator. It reads `_mssql_page` of every file of the table, which
  min/max prunes well for heaps and poorly for clustered tables after splits.
- It is phase A's path and the oracle. `MSSQL_DUCKLAKE_TEST_NATIVE_DELETE=1` runs every import through it,
  and the suite compares the two lakes row for row.

### D5. Schema evolution inside the commit

The mirror's catalog is diffed against the registry by object id and column id (E1 rule 6; E2 §4 for how
each change shows on disk). Each change is either a native DuckLake ALTER with stable field ids, or a
rewrite inside the same commit. Table and column ids are kept wherever DuckLake allows: renames keep
them (`DL/storage/ducklake_table_entry.cpp:785-810,1478`), and a DROP + ADD makes new ones.

| SQL Server change | seen as | lake action | rewrite |
| --- | --- | --- | --- |
| new table | new object, rowsets and AUs | `CREATE TABLE`, with 021 D5's spec and the `WITH` options | no |
| dropped table | object row gone; AUs gone or dead | `DROP TABLE`; its page-map rows deleted; registry status `dropped` | no |
| dropped and re-created under the same name (new object_id) | old object gone, new one present | `DROP` + `CREATE`, copying the lake table's **current** partition and sort spec (manual changes win) | no |
| table rename (object_id stable) | sysschobjs name | `ALTER TABLE … RENAME`, but only if the lake name is still the one the importer last set; a manually renamed lake table is left alone, with a notice (DQ-27) | no |
| `ALTER SCHEMA … TRANSFER` | schema id changed; pages untouched | DuckDB has no SET SCHEMA (`DD/src/include/duckdb/parser/parsed_data/alter_table_info.hpp:72-92`): `CREATE` in the target with the current spec, rows moved by a **lake-side export** (D8), `DROP` the source | yes (lake copy) |
| ADD COLUMN NULL (metadata-only) | new syscolpars and sysrscols rows | `ADD COLUMN` | no |
| ADD COLUMN NOT NULL DEFAULT k (metadata-only) | sysrscols status 0x200; k in `sysseobjvalues` by **hbcolid** | `ADD COLUMN c T DEFAULT k`: old files read k as the initial default (literal only, `DL/storage/ducklake_field_data.cpp:80-90`) | no |
| DROP COLUMN (metadata-only; bytes kept) | syscolpars row gone; sysrscols status 0x02, new rscolid | reset a partition or sort spec on it first (DuckLake requires it, `ducklake_table_entry.cpp:864-900`); `DROP COLUMN` | no |
| column rename | syscolpars name | `RENAME COLUMN` (field id kept); same manual-wins check as tables | no |
| int → bigint, decimal precision widening (size-of-data) | a new physical column, the old one kept as dropped | `ALTER COLUMN … TYPE` (a promotion DuckDB allows, `ducklake_table_entry.cpp:956-967,1106-1130`) | every page is in the file anyway |
| varchar / nvarchar length change (metadata-only) | syscolpars length | nothing: the lake has no length; the registry is updated | no |
| other type changes: scale, int → varchar, … (size-of-data) | a new physical column | `ADD` under a temporary name, `DROP` the old, `RENAME`; the rows are re-inserted from the file because every page changed | no lake copy |
| a type change DuckLake cannot express, where SQL Server did **not** rewrite the pages | the type changed, no new physical column | the table is rewritten from the lake by a lake-side export with the cast, into the new column | yes (lake copy) |
| NULL ↔ NOT NULL | sysrscols 0x80 | nothing; lake columns are nullable (021 D3, DQ-23) | no |
| computed column | syscolpars computed flag | persisted: a plain column (size-of-data); not persisted: registry text only (DuckLake refuses generated columns) | no |
| PK / clustered index created, dropped or rebuilt; heap REBUILD; a new partition scheme | new rowsets and in-row AUs; every in-row page new; the LOB_DATA AU kept (E2 R6) | nothing structural: old pages are in A, new pages in C. LOB values are reused by root bytes (D6). **Partitioning and sort order are not changed** (021 D5) | no lake copy |
| a SQL Server upgrade adds a base-table column | the mirror's schema differs | `ADD COLUMN` on the mirror table | no |

A **lake-side export** reads the source rows from the lake at S0, in `filename, file_row_number` order:

```sql
COPY (SELECT … FROM lake.s.t AT (VERSION => S0) [WHERE …] ORDER BY filename, file_row_number) TO …
```

It runs in the export phase (021 D2.2: a side connection, no lake transaction), writes new files with
their runs, and those are registered like any export. No native `INSERT … SELECT` runs inside the
registering transaction, so the transaction stays short. A DuckLake ALTER that the table's partition or
sort spec blocks resets the spec on that column first, and records the reset in the registry.

### D6. LOBs and the fragment map

Every row re-inserted carries a new `_mssql_lob` (021 D3). The reader (089) decides each value's source:

1. **The root is byte-identical to the lake's** (the same RID, or the same root bytes): the value and its
   map are copied from the lake. This also covers REBUILD, which keeps the LOB_DATA AU and does not rewrite
   its pages while every heap RID changes, so the match is by root bytes, not by RID (E2 R6).
2. **The root changed and every reachable node is in the file**: the value is read from the file, as after
   a whole-value UPDATE, which rewrites every fragment in place (E2 R8).
3. **The root changed and some nodes are absent**: the hybrid walk, as after a `.WRITE`, which leaves 736
   of 749 fragments of a 6 MB value out of the differential.
   - An absent node is looked up by node RID in the lake's maps.
   - A leaf takes its byte range from the old value's stored bytes, which come from the lake in the form
     DQ-01 chooses: stored bytes for every value (E1 rule 8), the lake value re-encoded and refused when
     not exact (089 D6), or re-encoded with `raw` for the exceptions (021 D3).
   - An internal node is expanded through its old children.

   This is exact because a page absent from a differential is unchanged since the base (E1 4.5; all six
   E2 cases byte-exact).
4. **A node in neither.** In a loaded differential, when the node lies on a page the recovery marks stale
   (090): the row keeps its old bytes **and its map**, and the reader reports the page `stale` with reason
   `lob`; the next file resolves it (E6 rule 8: dropping the map lost 3 values for good). In any other
   file the reader refuses the value by name (089 D6, E1 rule 7) and the file is not applied; E1 met it
   only in the forced wrong-base apply that C4 refuses.

**The side input** `<run>/base/lob.parquet` (089 D10), read at S0, holds `(object_id, _mssql_rid,
column_id, root, nodes, text_exact, value)` for every off-row value of every lake row the apply deletes,
i.e. the rows on pages of D. That includes a REBUILD's old rows (their pages are freed), whose values the
reader then matches by root bytes. Its size follows the LOB rows on deleted pages: 19–125 values per
differential in E1, and the whole table after a REBUILD (which SQL Server also paid). Row-overflow
values (`varchar(n)` off-row) take the same path. "Reuse the lake's value by `_mssql_rid`" is never used:
it failed 10 of 21 E1 steps, silently, and the damage survives an incremental full.

### D7. Pending and stale pages

The decisions are 090's, inside the reader. The storage and the lifecycle are this spec's: the lake keeps
the sets, passes them back as side inputs, and records the outcomes.

**Pending pages** come from any file whose tail had transactions open at LastLSN: pre-undo images plus the
transaction ids (021 D11; E6 §6.2). At the next file, 090 applies the refined rule to each page P, in
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
page's LSN in the next file exceeds W, and the page is in C. E6 carried 551 of 551 and 1,419 of 1,419 stale
pages. The apply checks it, since the stale set is lake state and not a reader input (089 D10): every page
of `stale_pages` must be `changed` (above W) or `freed` in the next file. **For a page that is neither, this
draft reports it by name, keeps it stale and proceeds; 090 makes it a named error at that file. The
disagreement is DQ-06.** A tail record older than the previous file's LastLSN, on a page the file lacks, is
already in the lake and is ignored. Rebuilding such a page from its FORMAT_PAGE lost 3 rows for good
(E6 rule 7, applied in 090 from `reflected_lsn`).

**Consistency, per import and per table**, from the reader's per-table status (090):

- `consistent`: `recovered`, and no pending page of the table remains;
- `pending`: `recovered`, with rows of transactions still open held back (pending pages remain);
- `stale`: committed work skipped until the next file (`stale`);
- a table the reader calls `unrecoverable` makes the file refused, naming it (090 D7).

It is reported in the summary, in `imports`, and in `ducklake_mssql_backup_history`. Before 090, a loaded
file is refused by default, as in 021 D11.

### D8. Partition maintenance (E7)

Incremental imports never change a table's partitioning or sort order (021 D5): after creation the layout
is DuckLake's (owner, 2026-11-05, DQ-10). New rows land under the lake table's current spec, so a SQL
Server repartitioning only changes which DuckLake files the rewritten rows go to, and SQL Server's
partition numbers are never stored with the rows (E7§5). A spec set at creation stays valid after SPLIT /
MERGE: every row's value is computed from the row itself (after E7's s08 a fresh proposal would be
`epoch_year`, and the `epoch_month` spec set at creation keeps holding).

E7 replayed fifteen one-operation differentials with the rules below, exact in every one (E7§5):

| SQL Server operation | in the differential (089 § Partitioning) | in the lake |
| --- | --- | --- |
| SWITCH out, in, partition → partition | no data page; the two rowsets swap `idmajor` (and `numpart`) | step 5 of D3: the rows of the moved AUs are copied lake-side into the target and deleted by position in the source |
| SWITCH of a columnstore partition | no page; the columnstore and delete-bitmap rowsets swap | the same, by rowset (`page_map.au_id` holds the columnstore rowset) |
| SWITCH, then DML on the switched rows | only the DML'd pages | those pages by the page rule, re-read under the new owner; the rest by step 5 |
| NEXT USED; SPLIT or MERGE of empty ranges | catalog pages only | mirror rows only |
| SPLIT, populated | the moved rows on new pages of a new rowset; the emptied pages freed | positional deletes of the freed pages' rows; new files, under the current spec, for the moved rows |
| MERGE, populated | the right partition's rows on new pages of the left rowset; its pages freed and its AUs gone | the same; a file left with no live row is dropped (D4) |
| TRUNCATE … WITH (PARTITIONS), small | pages freed; none in the diff | positional deletes from `freed` |
| TRUNCATE … WITH (PARTITIONS), big | the old AU type 0 / owner 0 (`dead_au`) with its pages still allocated; later `gone_au` and freed pages | positional deletes by AU: the dead-AU clause of D3 step 4 (without it E7 kept all 9,000 rows) |
| REBUILD PARTITION | every row on new pages; new rowsets and in-row AUs; the LOB_DATA AU kept and re-owned | delete and re-insert; LOB values by root bytes (D6 step 1; 24 values in E7) |

**SWITCH, with DuckLake means (E7's ownership rule; owner, 2026-11-05).**

- A SWITCH writes no data, LOB or index page: the rowsets exchange `idmajor` (and `numpart`), the AUs stay
  with their rowsets, page headers and IAMs are unchanged, no DCM bit is set (E7, five variants). The page
  rule alone leaves the rows in the old table: E7 left 560 rows in `m_ci`, and after a SWITCH and a one-row
  UPDATE only the 37 rows of the touched page moved and 263 stayed.
- **The rule**: after the mirror update, every lake row's recorded AU (columnstore: rowset) is resolved
  through `sysallocunits.ownerid` → `sysrowsets`. When the (object, index) differs from the lake table the
  row is in, the row moves; when only `numpart` differs, nothing happens (SPLIT / MERGE renumbering). The
  reader reports the moved AUs as `au_moved` (089). This is why the page map records the AU per page, not
  the table (021 D3).
- **The move is a lake-side copy plus positional deletes, never a file move.** The export phase reads the
  source rows of the moved AUs from the lake at S0 (`page_map.au_id` ∈ M, minus the pages of D, which the
  page rule re-reads), in `filename, file_row_number` order, and writes them as new files of the target
  table in its current DuckLake layout, with new runs. The apply registers them and deletes the source
  positions (D3 step 5). Columns map by name. The moved rows keep their `_mssql_rid`, `_mssql_page` and
  `_mssql_lob`, because the pages did not change.
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

021 writes a clustered columnstore table's compressed rows as runs of surviving tuples (`page_map.kind = 1`,
088 D3): tuple *t* of rowgroup *g* sits at `first_pos + (t − first_tuple)` of the run that holds it, and a
tuple in no run was already deleted when it was exported. Delta-store rows are ordinary pages (`kind = 0`).

- **Delete bitmap.** Its pages are rowstore pages (PAGE / CD records, E5 §3.7). For each bitmap page in C,
  every entry (g, t) on it becomes the position of t in its run; an entry for a tuple in no run needs
  nothing. Entries already applied merge idempotently (D4), so the importer needs no bitmap history.
  Entries leave the bitmap only with their rowgroup.
- **New compressed rowgroups** (new `syscsrowgroups` rows in the mirror) are exported: their segments are
  newly written LOB pages, so they are in the file.
- **State transitions**:
  - the tuple mover compressing a delta store frees the delta's pages (U: its rows go) and adds a
    rowgroup (inserted);
  - a rowgroup that turned TOMBSTONE, or vanished in a REORGANIZE or REBUILD, is in A (`dead_rowgroup`),
    and its runs' positions are deleted (a file left with no live row is dropped);
  - a TOMBSTONE's delta store can still own allocated pages, so the reader reports it as a dead rowset,
    not as data.
- **A nonclustered columnstore index** is an index: the table's data is its rowstore, so nothing is
  imported from it.
- **Ordered CCI**: whatever sort spec the table has (proposed at creation or not, DQ-25) stays.
- **A partitioned CCI's SWITCH** moves its rows by rowset (D8); its SPLIT / MERGE waits for E11.
- **Open**: the tuple mover running during a backup is untested (E5 §6.6; 088's E11 measures it), and so
  are segment pages touched in a loaded tail. 090 D7 makes such a table `unrecoverable`, so the file is
  refused; this draft proposed marking the table stale for that file instead (question 4). The
  disagreement is DQ-07.

### D10. A later full, and rebase

- **Applied incrementally** (the default). The changed set is the full's pages with LSN > W: 227 of 1,244
  in E1's F1. The freed set is complete, because a full carries every allocated extent and every PFS page.
  Every pending page resolves, every stale page heals, a non-copy-only full becomes the new base, and a
  generation starts (C5; whether a copy-only full starts one too is DQ-16). The result equals a fresh import of the same full (E1 and E6: identical lakes,
  `__mssql_sys` and locators included).
- **It repairs nothing.** It reads only changed pages, so earlier damage survives (E1 rule 10).
- **`mode := 'rebase'`** reads every page of the full whatever its LSN: every row of every imported table is
  deleted and re-inserted. That costs a fresh import's writes, in the same lake, with history kept. It is
  for:
  - after a recovery fork (C1);
  - after a change of the import filters (021 D1);
  - for a SWITCH in from outside the filters (D8);
  - for repair.

  The plan shows its size first.

### D11. The commit for a file

**Calls** (021 D7, plus):

- `AddDeletes`;
- `DropFile`;
- `DuckLakeDeleteFileWriter::WriteDeleteFileWithSnapshots`;
- `DuckLakeDeleteFilter::ScanDeleteFile`;
- the DDL of D5, through the expansion's statements.

**What DuckLake writes**, on top of 021's list:

- a `ducklake_delete_file` row per rewritten delete file, and the end of its predecessor;
- `ducklake_files_scheduled_for_deletion` for superseded delete files and dropped data files;
- `end_snapshot` on dropped data files;
- per touched table, the stats refresh (spec 014's CTE `UPDATE`, in 1,000-row chunks);
- the ALTERs' column rows.

The commit takes the client loop: the server-side apply covers data-files-only commits, and this one has
deletes (`MD/mssql_server_commit.cpp:258-262`).

**Volume.** Rows ≈ F·(1 + C) + Fd·3 + Fx·2 + Tt·(1 + C) + DDL + P·8, where Fd = delete files rewritten,
Fx = files dropped, Tt = tables touched. A typical differential has 50 changed tables, 2 new files and 3
delete files each, and 10 mirrors changed: about 8 k rows. That is a few `mssql_exec` round trips and the
appender's bulk loads, under a second to a few seconds (estimate). A hot data file's cumulative delete file
is rewritten at every import that touches it, so its size grows with the positions deleted. The
compaction policy rewrites files past the delete threshold (021 D6, 023 §D5).

**Conflicts.** The import's deletes conflict with a concurrent delete on, or compaction of, the same file,
and with any concurrent insert into a table it deletes from
(`DL/storage/ducklake_transaction_state.cpp:295-305,2480-2560`). A user writing to an imported table at
the same time makes the import fail at commit, and nothing is lost: the run is resumable.

### D12. Writes by others to imported tables

The importer is the only writer of imported tables, `__mssql_sys` and `__mssql_backup`. A user insert puts
rows into a table that no page map knows, and they would survive forever. A user update is a delete plus an
insert of rows still naming their old `_mssql_page`, so the next page delete would miss them.

So the callback lists the snapshots since L from `ducklake_snapshots` (`changes_made`), excluding the
importer's own and its compactions, and checks what they touched:

- `inserted_into`, `deleted_from`, inlined inserts or deletes, or an ALTER of an imported table other than
  partitioning, sort, options, comments or a rename: refused with `foreign_writes := 'error'`, naming the
  snapshot, its author and the table; `'ignore'` accepts the divergence and records it;
- a compaction: allowed, and the page map is rebuilt (D3 step 0);
- a manual DROP of an imported table: refused, with "re-create it, or exclude it from the filters and
  rebase".

## Enforcement & security

- **Fail closed** on every chain rule (D2), on a plan-id mismatch, on a foreign write, on a stale page the
  next file cannot heal (reported, the import proceeds and keeps it stale), and on an unresolvable LOB
  (kept old with its map; reported). Nothing is guessed: a refusal names the file, the GUIDs or LSNs, the
  table and the snapshot.
- **No dirt.** Rows of open transactions never reach a user table (090, D7). The pending images do live in
  `__mssql_sys.pending_pages` and in its history until expiry (DQ-09).
- **Side inputs and run files** (`base/catalog/`, `base/allocation.parquet`, `base/pending.parquet`,
  `base/lob.parquet`, `deletes.parquet`, the moved rows' copies) contain database
  data. They are written in the run directory under the lake's data path, with the lake's protection, and
  deleted after the commit (021 D9).
- **Trust in the reader.** Every decision about pages comes from the file plus the lake's own state, never
  from user input. The page map is checked against the files before it is used (step 0).
- **Version mismatches.** A DuckLake bump re-audits the delete path, which mirrors `FlushDeleteWithSnapshots`.
  A reader version change between imports is allowed. The imports record it, and a mirror schema change is
  applied as D5's last row.

## Testing

**The oracle** is 021's: at every snapshot, RESTORE of the same chain prefix against `AT (VERSION => n)`,
per table, multiset over canonical rows, plus locators. Every chain also runs twice, with positional
deletes and with the native fallback (`MSSQL_DUCKLAKE_TEST_NATIVE_DELETE=1`), and the two lakes must
agree row for row.

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

- drop and re-create under the same name, keeping a manually changed spec;
- TRANSFER;
- a rename after a manual lake rename (the manual name wins);
- a metadata-only type change that DuckLake cannot express.

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
- the same bitmap page carried by two files (idempotence).

**Partition maintenance (E7)**: E7's maintenance database and its fifteen one-operation chains
(`experiments/E7/sql/b00`–`b02`, `s01`–`s14`: SWITCH out, in, partition → partition, columnstore, SWITCH
followed by DML and by a one-row UPDATE, NEXT USED, SPLIT and MERGE empty and populated, TRUNCATE
PARTITIONS small, deferred and 30 s later, REBUILD PARTITION), each table compared with RESTORE; plus
staging tables outside the filters (SWITCH out deletes, SWITCH in is refused naming rebase), RANGE LEFT
SPLIT / MERGE, and a SWITCH out and back inside one differential's interval (nothing moves). The lake
tables keep the spec they were created with, whatever SQL Server does.

**Writes by others**: a user INSERT, DELETE or ALTER on an imported table makes the next import refuse. A
user-forced compaction gets the page map rebuilt, and the oracle reports 0.

**Counterfactuals.** Each rule is switched off alone (test-only switches, in mssql for the reader's rules
and in mssql-ducklake for the lake's), and the expected failure is the measured one:

| rule switched off | expected | source |
| --- | --- | --- |
| frees from the PFS (changed pages only) | `t_trunc` keeps its pre-TRUNCATE rows: 850 against 50 | E1 4.2 |
| R3 liveness (`Size` + GAM + PFS) on row sources | after SHRINKFILE, 4,000 rows against 2,000 | E2 §2 |
| delete by page in every table (in the owner only) | row 801 of a truncated table survives on a page reused by `t_pk` | E1 4.3 |
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
| the page-map rebuild after a compaction | wrong rows deleted after a `CHECKPOINT` on an imported table | 021 D6 |
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
- **Native DELETE as the main path**: its cost follows the lake's size. It is kept as the fallback and
  the oracle.
- **Keeping raw record bytes per row** so that a loaded differential is exact on its own (the E6 oracle run
  shows it would be): it roughly doubles storage, and the owner chose variant 1.
- **A metadata-only SWITCH, or moving the source's whole files to the target**: DuckLake has no file move
  and no reference-counted cleanup (D8), and the owner chose DuckLake means: a lake-side copy plus
  positional deletes (2026-11-05, DQ-10).

## Open questions for the owner

All are in [decisions.md](https://github.com/hugr-lab/mssql-extension/blob/main/specs/085-backup-reader-core/decisions.md), merged with the mssql series and ordered by impact:

| # | question | DECISIONS |
| --- | --- | --- |
| 1 | a lake born from a copy-only full (C6) | DQ-15 |
| 2 | renames on both sides | DQ-27 |
| 3 | offer `loaded_diff := 'partial'` at all | DQ-17 |
| 4 | columnstore under load: mark stale (drafted here) or refuse the file (090 D7) | DQ-07 |
| 5 | writes by others refused by default (D12) | DQ-08 |
| 6 | `mode := 'rebase'` in v1 | DQ-14 |

Added by the consistency pass: DQ-06 (a stale page the next file neither carries nor frees: error, as
090 drafts, or report and proceed, as D7 drafts) and DQ-16 (which fulls start a generation; C5 against
023 D1).

## Follow-ups

- **A verify mode**: a fresh import of a full into a scratch lake, compared with the lake's state at the same
  backup, without writing (E1 rule 10's insurance), reporting the differing keys per table.
- **Row-level change minimisation**: today every row of a changed page is deleted and re-inserted, so
  `ducklake_table_changes` shows unchanged rows as delete + insert. Keeping unchanged rows (comparing by
  `_mssql_rid` and content) would make the change feed exact. It needs the old rows read back.
- **Log backups** (RESEARCH phase 4): the same page apply, driven by redo of a log chain.
- **External workers for differentials** (021 D4 already allows it; the side inputs must reach the
  workers).

## Tasks

| # | task | estimate |
| --- | --- | ---: |
| 1 | Chain validation C1–C7 in the callback and the apply; `if_applied`; fork detection; the refusal messages | 3 d |
| 2 | The changed, unallocated and dead sets from `mssql_backup_changed_pages`; the global page-map lookup; `deletes.parquet` | 3 d |
| 3 | The `base/` side inputs (catalog, allocation, pending, LOB); the mirror apply; the catalog diff against the registry → the DDL decision list | 5 d |
| 4 | Schema evolution: native ALTERs (D5), the manual-wins checks, lake-side exports for TRANSFER and inexpressible changes | 6 d |
| 5 | Positional deletes: the `FlushDeleteWithSnapshots` path, `DropFile`, retries; the native fallback and its switch | 6 d |
| 6 | Page-map maintenance: stale rows, new runs, the rebuild after a compaction (with 021 task 11) | 3 d |
| 7 | LOBs: `base/lob.parquet`, reuse by root, REBUILD by root bytes, an unresolvable value kept with its map in a loaded file | 5 d |
| 8 | Pending and stale tables and their lifecycle with 090: the outcomes, the skip default, the heal assertion, consistency reporting | 6 d |
| 9 | Partition maintenance (E7): SPLIT, MERGE, TRUNCATE, REBUILD PARTITION by the general rules; SWITCH by the ownership rule (`au_moved` → lake-side copy into the target + positional deletes in the source); E7's fifteen chains and the two counterfactuals | 5 d |
| 10 | Columnstore: bitmap → positions, rowgroup transitions, dead rowsets (after 088) | 5 d |
| 11 | Writes by others: detection from `ducklake_snapshots`, refusal, rebuild on a compaction | 2 d |
| 12 | Later fulls and `rebase` | 2 d |
| 13 | The oracle runs: E1 chains, E2 edges, E6 cycles, schema-evolution and partition scenarios, the counterfactual switches and their assertions, both delete paths | 10 d |
| 14 | Docs page and examples | 2 d |
| | **total** | **63 d ≈ 12.5 wk** (62 before E7's SWITCH rule and tests joined task 9) |

By phase (README):

- **I3**, needing 089: tasks 1–7, 9 (rowstore SWITCH included, now that E7 is done), 11–12, and most of 13.
  About 9 weeks.
- **I4**, needing 090: task 8 and the E6 part of 13. About 2–3 weeks.
- **I5**, needing 088: task 10 and the columnstore SWITCH by rowset. About 1–1.5 weeks.

The README's 11–13 weeks allows for overlap.
