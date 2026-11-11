# Spec 025: frozen history, rows the source trims and the lake keeps

- **Status**: draft
- **Date**: 2026-11-10
- **Author**: vgsml, with Claude

Builds on [022](../022-import-full-backup/spec.md) and [023](../023-incremental-import/spec.md)
(PR #56) and changes their apply for opted-in tables. The one thing it needs from mssql-extension is a row
filter on the export (§ What mssql-extension provides).

## Summary

Large history tables in a source database are routinely trimmed: a job deletes `events` older than
90 days, or switches out and truncates the oldest partition. 022–024 mirror the source, so the next import
deletes the trimmed rows from the lake too. History survives only in old snapshots, which 024 expires.

This spec lets the lake keep those rows. A table opts in with a **freeze column** (a time or another
ordered column) and an interval. Before each import's deletes, rows older than the freeze boundary are
**frozen**: their `_mssql_page` is set to `'FROZEN'`. Nothing in the import deletes by anything but the
page, so from then on no import touches them. The export leaves rows below the boundary out, so a
source row that still exists, and comes again after an index rebuild or in the next full, is not
inserted twice. The table stays one table: current rows plus frozen history, queried as usual.

## Problem

- **Trimming in the source deletes history in the lake.** It arrives as freed pages, dead or gone
  allocation units, a truncated partition or changed pages, and each becomes a
  `DELETE … WHERE _mssql_page IN (…)` (023 D3 step 4, D4). The trim cannot be told apart from a
  business delete or from rows that only moved (below).
- **Old snapshots are not history.** Rows trimmed in the source are readable only `AT (VERSION => n)` of
  an import before the trim. "All events ever" is then a union over versions with deduplication. Keeping
  those snapshots also keeps every superseded file alive, and 024's retention exists to drop them.
- **Page identity cannot mark history.** A page is a physical address, not a point in time. Freed pages
  and extents are reallocated to any object, and in a trimmed table they are its oldest pages, the ones
  trimming frees. Rows also change pages without changing: page splits, REBUILD and REORGANIZE, SHRINK,
  heap forwarding. Several files in a filegroup interleave allocations (proportional fill). So "freeze
  page numbers up to N" loses new rows that land on reused pages, and duplicates old rows that move.
- **The kind of a delete does not mean "gone from the source" either.** A REBUILD frees every old page
  and kills the old allocation unit while every row survives on new pages. So freezing the rows of
  `freed` or `dead_au` pages instead of deleting them would duplicate the whole table at the next
  rebuild.
- **Snapshot tags do not help.** DuckLake's tags (`ducklake_create_tag`, duckdb/ducklake#1851, on `main`
  in catalog format `2.0-dev1`, not in 1.1) only protect a snapshot from expiry. They do not stop a later
  DELETE from removing rows from the current state.

## Design

### D1. Opting a table in

```sql
CALL ducklake_mssql_backup_set_freeze('lake', 'dbo.events', column => 'ts', after => INTERVAL 80 DAY);
CALL ducklake_mssql_backup_set_freeze('lake', 'dbo.events', NULL);   -- off; frozen rows stay frozen
```

Stored in `__mssql_backup.config` per table:

| key | meaning |
| --- | --- |
| `freeze.column_id` | the freeze column, by SQL Server column id, so a rename keeps it (D6); shown with its current name |
| `freeze.after` | an interval for a time column, a number for an integer column (an identity, a sequence) |
| `freeze.upto` | the table's boundary so far: frozen are exactly the rows with `column <= upto`; NULL before the first freeze |
| `freeze.upto_snapshot` | the snapshot that last raised it |

- **Freeze column types**: `date`, `smalldatetime`, `datetime`, `datetime2`, `datetimeoffset` (compared as
  the instant) and the integer types. Rows with NULL in it are never frozen.
- **Not freezable**: the `__mssql_sys` mirrors and `__mssql_backup`, and a table without such a column.
- **The interval is chosen against the source's retention.** It must be shorter than how long the source
  keeps rows, with a margin: for a 90-day source, 80 days. A row then freezes while the source still has
  it, so the trim later finds it frozen. A longer interval than the source's retention loses rows. The
  importer cannot see the source's retention, so this is documented and set by the owner.

### D2. The boundary, per import

For an import of file f, per opted-in table:

```text
upto_f = max(upto_previous, f.BackupFinishDate - after)        -- a time column
upto_f = max(upto_previous, max(column) over live rows - after) -- an integer column, from the lake at S0
```

- **It never decreases.** A row frozen once stays frozen, and the export filter (D4) never readmits it.
- **The backup's own time, not `now()`.** Catching up 30 differentials at once, or running an import
  again, gives each file the boundary its own time gives. `BackupFinishDate` is the time the file is
  consistent at (023 D2: the reader recovers to the end); 085 has it as
  `mssql_backup_info(path).backup_finish_date`, a TIMESTAMP in the source server's local time, as
  RESTORE HEADERONLY prints it. HEADERONLY has no time-zone column, and whether the backup set records
  the server's offset elsewhere is not measured yet (mssql-extension is checking). Until then the time
  is taken as server-local with no offset; the freeze column may hold UTC, and the margin in `after`
  covers the difference.
- **Computed in the plan, at S0** (023 D3), written into `plan.json`, and passed to the export (D4). The
  commit writes the new `upto` with the import, in the same transaction, so a retry or a resumed run uses
  the same boundary.

### D3. The freeze, in the import's transaction

023 D3's expansion gains one statement per opted-in table, after the validation and the DDL of D5,
before the first `DELETE`:

```sql
SELECT * FROM ducklake_mssql_backup_validate('lake', '…', plan := '<run>/plan.json');
ALTER TABLE …;                                                      -- 023 D5, D6 below
UPDATE lake.dbo.events                                              -- the freeze
   SET _mssql_page = 'FROZEN', _mssql_rid = NULL, _mssql_lob = NULL
 WHERE ts <= :upto AND _mssql_page <> 'FROZEN';
DELETE FROM lake.dbo.events WHERE _mssql_page IN (…);               -- 023 D4: frozen rows never match
INSERT INTO … SELECT … WHERE _mssql_page IN (…); DELETE …;          -- 023 D8, a SWITCH: frozen rows never move
SELECT * FROM ducklake_mssql_backup_apply('lake', '…', manifest := …, plan := …);
```

- **One snapshot, as before.** The freeze is the importer's own write, inside the import's snapshot, so
  023 D12's foreign-write check does not see it.
- **DuckLake's UPDATE is a delete and an insert.** The frozen rows are rewritten once in their life into
  new files, and the old files go through the usual delete files and compaction. Most imports freeze a
  day's worth of rows.
- **`'FROZEN'` works with the page filter.** `_mssql_page` values are fixed-width uppercase hex starting
  `01` or `02` (022 D3). `'FROZEN'` sorts after all of them under the catalog's BIN2 collation, so a
  file of frozen rows (min = max = `'FROZEN'`) is pruned by every delete by page (to verify, with 022
  task 11's pruning check).
- **An UPDATE after the DDL in one transaction**, on the table the DDL changed: 023 already runs its
  DELETEs after its ALTERs, and the freeze relies on the same (to verify for UPDATE).
- **Why clear `_mssql_rid` and `_mssql_lob`.** A frozen row has no page in the source any more. Its row
  locator and LOB fragment map would otherwise remain candidates for 023 D6's LOB reuse and D9's
  delete-bitmap deletes. Its values are already whole in the lake's columns.
- **Every delete path is covered by construction.** Each one selects by page: 023 D4 (all its kinds), D8
  (SWITCH), D9 (columnstore rowgroup keys plus `_mssql_rid`). A truncated partition or a switched-out
  partition of trimmed rows therefore leaves the frozen rows where they are, which is the point.
- **A standalone freeze**, `ducklake_mssql_backup_freeze('lake', 'dbo.events')`, raises the boundary
  now: the same UPDATE in its own snapshot, authored by the importer, with the importer's commit message
  so D12 counts it as the importer's own.

### D4. The export leaves frozen rows out

The source still holds a frozen row until it trims it, and the row comes back whenever its page is in the
changed set. An index rebuild sends every page; a full applied incrementally (023 D10) sends every page
past W; a rebase sends all of them. So the export writes, for an opted-in table, only rows with
`column > upto_f` or `column IS NULL` (§ What mssql-extension provides).

- **Each row is in the lake once.** At import k a row is either above the boundary, and then it is live
  and handled by page as in 023, or at or below it, and then its lake copy is frozen and the export drops
  its source copy. Raising the boundary between imports moves rows from the first case to the second,
  which the freeze of D3 does before the deletes.
- **Lost by design: rows that arrive below the boundary.** A row inserted into the source after the
  boundary passed its value (a back-dated or late event) is never exported. The interval's margin must
  exceed the source's lateness. The export reports per table how many rows it left out (`rows_frozen_out`).
  Telling a late row from a frozen row's copy needs a key (Follow-ups).
- **Lost by design: changes to frozen rows.** An UPDATE in the source of a row at or below the boundary
  does not reach the lake. That is what freezing means; a table whose old rows change is not a candidate.

### D5. Rebase and a later full

- **Applied incrementally** (023 D10), a full is just a file with many changed pages: D3 and D4 apply
  unchanged.
- **`mode := 'rebase'`** deletes and re-inserts every row of every imported table (023 D10). For an
  opted-in table it deletes only live rows, `WHERE _mssql_page <> 'FROZEN'`, and the export's filter holds
  as always. The frozen history survives a rebase, a recovery fork included.
- **A dropped and re-created source table** (a new object id; 023 D5) is a new lake table. The old one's
  frozen rows are kept as `<table>__frozen_<snapshot>`, unless the re-created table has the same columns
  and the owner chose `freeze.carry := true`. Then they move into it (to decide, DQ below).

### D6. Schema changes with frozen rows

023 D5 maps the file's catalog diff to DDL inside the import's transaction. Frozen rows are not in the
source any more, so **whatever a change does to them is done in the lake, never by reading the source
again**. One file always carries one schema, the one at its end (the reader recovers to the end, 090), and
between two files only the net change is visible. A column added and dropped between two differentials
is no change.

| change in the source | live rows (023 D5) | frozen rows |
| --- | --- | --- |
| ADD COLUMN, NULL or a constant default | `ADD COLUMN … DEFAULT` | the same statement: DuckLake reads old files with the column's `initial_default` (`DL/storage/ducklake_table_entry.cpp:1078-1082`), which is SQL Server's value for existing rows |
| ADD COLUMN with a non-constant default (`newid()`, …) | SQL Server rewrites every row, all re-exported | NULL: they never had a value; documented |
| a promotion DuckLake allows (`int` → `bigint`, `real` → `float`, wider `varchar`) | `ALTER COLUMN … TYPE`, metadata only (`TypePromotionIsAllowed`, `DL/storage/ducklake_table_entry.cpp:956`) | the same statement |
| any other type change (`int` → `varchar`, a decimal's scale) | 023 D5: a new column under a temporary name, the rows re-inserted, the old one dropped, renamed | converted in the lake in the same transaction: `UPDATE … SET <temp> = CAST(<old> AS <new>) WHERE _mssql_page = 'FROZEN'` before the drop; a failing cast refuses the import, naming the table and column |
| RENAME COLUMN | rename | the same statement |
| DROP COLUMN | **kept** in the lake on an opted-in table (`freeze.drop_columns := 'keep'`, the default): live rows get NULL from now on, frozen rows keep their values; `'drop'` follows the source | as live |
| the freeze column dropped or retyped to a non-orderable type | the import is refused, naming the table: change the freeze setting or turn it off | — |

The freeze column's `column_id` survives a rename. A retype that stays orderable (`datetime` →
`datetime2`) keeps the boundary, converted.

### D7. Retention

With history in the frozen rows, 024's expiry no longer has to keep old generations for the sake of
history. Expiring the snapshots between freezes drops the superseded files and keeps every frozen row. 024
needs no change. Its documentation should say that frozen tables keep history independently of the
policy.

## What mssql-extension provides

The export is mssql's `mssql_backup_export` (089, called through `ducklake_mssql_backup_export`, 022
D2.2), which writes the data files itself. It needs one addition:

- **`exclude_upto := MAP {object_id: {'column_id': …, 'upto': …}}`.** A decoded row of that object whose
  column value is not NULL and is `<= upto` is not written.
  - The comparison is the column's SQL Server ordering. `datetimeoffset` is compared as the instant.
  - The manifest's row counts and column statistics describe the written rows only.
  - The result gains `rows_excluded` per unit.
  - External workers (022 D4) get the map from `plan.json` like every other argument.
- **Already there**: `backup_finish_date` in `mssql_backup_info` (085). Still open: whether the backup
  set carries the server's time zone or offset (mssql-extension is measuring it).

Nothing else: the freeze, the boundary and the schema rules are this repository's.

## Enforcement & security

- **The boundary never decreases** (D2); a manual setting that would lower it is refused.
- **Turning a table's freeze off keeps its frozen rows.** They stay out of every delete, and the export
  filter stays at the last boundary, so they are never doubled. Unfreezing rows is not offered: it would
  need their pages back.
- **A user's UPDATE of `_mssql_page`** is a foreign write and refused (023 D12), as any user write to an
  imported table is.
- **The export filter and the freeze always use the same `upto`**, the plan's. A plan whose `upto`
  differs from the commit's config is invalid and is re-planned (022 D2.3's validation).

## Testing

On the E1/E7 corpus where possible, with fixtures built on a real SQL Server (the mssql-ducklake-*
container):

- **Trim, then full.** Rows older than 85 days are trimmed in the source between two fulls, with a
  91-day-old row in between. After the second full the frozen rows are all present, and live rows match
  the source exactly.
- **Rebuild after a freeze.** `ALTER INDEX … REBUILD` between two differentials, after the freeze. Not
  one frozen row is duplicated (`count(*)` per key). The counterfactual is the same run with the export
  filter switched off (a test-only switch): every frozen row still in the source appears twice.
- **Trim by partition**, SWITCH OUT plus TRUNCATE, and by `DELETE`, which leaves ghost records and later
  freed pages: both keep the frozen rows. The counterfactual: without the freeze, both delete them.
- **Late row below the boundary.** Not imported, and `rows_frozen_out` counts it.
- **Catch-up.** Thirty differentials imported in one go give the same lake as one at a time. The boundary
  follows each file's own time.
- **Schema**: one case per row of D6 with frozen rows present, the failing cast included. A rename of the
  freeze column keeps freezing.
- **Rebase** keeps the frozen rows, with no duplicates.
- **Retention**: expire every snapshot but the last, and the frozen rows are still all there.
- **Pruning**: a delete by page reads no file whose rows are all frozen (the file counts, as in the
  0659d57 review's measurement).

## Alternatives considered

- **A per-import row predicate on the delete only**, without freezing. Every delete path needs the
  predicate (D4, D8, D9 and rebase), and one missed path loses history. The export filter is needed
  anyway.
- **An archive table** (rows that vanish from the mirror are copied out by key, with a view over both).
  It handles late rows and needs no interval, but it needs a business key on every table. It also adds a
  second table, and runs an anti-join in every import.
- **A page-number watermark** and **freezing by delete kind** (`freed`, `dead_au`): both wrong, see
  Problem.
- **DuckLake snapshot tags or branches**: tags protect snapshots, not rows. Branching
  (duckdb/ducklake#1888, open) is about file visibility per branch and has no row-level merge.

## Decisions to make

| # | question | proposal |
| --- | --- | --- |
| 1 | `DROP COLUMN` on an opted-in table | keep the column (`freeze.drop_columns := 'keep'`) |
| 2 | a dropped and re-created table's frozen rows | keep them as `<table>__frozen_<snapshot>`; `freeze.carry` to move them when the columns match |
| 3 | the time zone of `BackupFinishDate` against the freeze column | the margin in `after`; an explicit `freeze.source_tz` only if 085 cannot read the server's offset |

## Follow-ups

- **Late rows by key.** For a table with a primary key, the export compares rows below the boundary
  against the frozen keys instead of dropping them. A key not found is a late row and is inserted frozen.
  That costs an anti-join, heavy only after a rebuild.
- **Detecting a source that trims ahead of the freeze** needs the same key. Without one, a trimmed row
  is indistinguishable from a moved one (Problem).
- **On a catalog format with tags** (`2.0-dev1` and later), tag the snapshot that raises a boundary,
  for an easy `AT (VERSION => 'freeze-…')`.
