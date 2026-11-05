# Spec 024: backup retention and time travel

- **Status**: draft (number tentative)
- **Date**: 2026-11-05
- **Author**: vgsml, with Claude

## Summary

Every imported file is one snapshot (022, 023), so `AT (VERSION => n)` is the database at a backup. This
spec does three things with that history:

- **It groups it into generations**: the snapshots from one applied full up to the next.
- **It keeps what the owner's policy keeps**: the last *m* generations, and the last *n* import snapshots
  within them.
- **It expires the rest**: `ducklake_mssql_backup_expire` computes the snapshot ids from the importer's
  own tags, then calls DuckLake's `ducklake_expire_snapshots(versions := …)` and
  `ducklake_cleanup_old_files`.

The policy can be stored in `__mssql_backup.config` and run after every successful import. A helper maps a
backup time to its snapshot. The spec also states how retention and the importer's compaction work
together. Only import snapshots are counted and expired; snapshots made by users or by compaction are left
alone unless an option says otherwise.

## Problem

- **Snapshots pile up.** A lake fed daily differentials and weekly fulls gains about 365 snapshots a year.
  Each one keeps its files alive: replaced data files, superseded delete files, old page-map runs, and
  pending page images. Once a newer full is applied, the older generations are only history. The lake's
  current state plus the new chain is all that future imports need (RESEARCH §6.2), so that history can go
  when the owner says.
- **DuckLake expires by age or by explicit ids** (`DL/functions/ducklake_expire_snapshots.cpp:63-81`). It
  cannot tell an import from a user's commit or a compaction. "Keep the last four fulls" is not expressible
  as an age.
- **`CHECKPOINT` can expire imports.** It runs `ducklake_expire_snapshots(lake)`
  (`DL/storage/ducklake_checkpoint.cpp:20-24`) with the lake-wide `expire_older_than`. If that option is set,
  CHECKPOINT expires import snapshots by age, fulls included.
- **Time travel by backup time is not a DuckLake notion.** `AT (TIMESTAMP => t)` is the commit time of a
  snapshot, which may be hours or days after the backup was taken, and meaningless after a bulk catch-up
  import.

Paths as in 022.

## Design

### D1. Generations and tags

Every import writes, in its own snapshot (022 D8):

- the `__mssql_backup.imports` row, which holds `import_seq`, `generation`, `backup_type`,
  `backup_set_guid`, `differential_base_guid`, `backup_start_date`, `backup_finish_date`, the LSNs and
  `consistency`;
- the commit's `extra_info` (`"kind": "mssql_backup_import"`).

The row's snapshot is its `snapshot_id` virtual column, which still answers after the snapshot has expired.
**`imports` is the authority**: `ducklake_snapshots()` stops listing a snapshot once it expires.

- `generation` = 1 at the first import.
- It increments when the applied file is a full, copy-only included (as drafted; 023 C5 had said "a
  non-copy-only full starts a generation", so which fulls count is DQ-16 in [decisions.md](https://github.com/hugr-lab/mssql-extension/blob/main/specs/085-backup-reader-core/decisions.md)).
- Differentials keep the generation they were applied in.

The first snapshot of a generation is its **full snapshot**.

### D2. The policy

Inputs: `keep_fulls` = *m* (≥ 1, or NULL for unlimited) and `keep_snapshots` = *n* (≥ 1, or NULL for
unlimited). Let I be the import snapshots that still exist, in snapshot order, and G their distinct
generations.

1. G_keep = the last *m* generations of G.
2. **Expire every import snapshot of a generation outside G_keep.**
3. Within G_keep, **keep each generation's full snapshot and the last *n* import snapshots** (counted over
   G_keep, DQ-30). Expire the other import snapshots.
4. **Never expired:**
   - the latest import snapshot, which is why *n* ≥ 1;
   - the lake's latest snapshot (DuckLake's own rule, `DL/functions/ducklake_expire_snapshots.cpp:63-65`).
5. **Non-import snapshots** (user commits, compactions, the config changes of D4) are neither counted nor
   expired. With `include_other := true`, those older than the oldest kept snapshot are expired too.

Example: daily differentials and a full every Sunday, with `keep_fulls := 4, keep_snapshots := 14`. That
keeps the last four Sunday snapshots and the last fourteen days, and nothing older.

### D3. `ducklake_mssql_backup_expire`

```sql
CALL ducklake_mssql_backup_expire('lake', keep_fulls := 4, keep_snapshots := 14, dry_run := true);
```

| option | default | meaning |
| --- | --- | --- |
| `keep_fulls`, `keep_snapshots` | the stored policy (D4), else required | D2 |
| `dry_run` | `false` | list the decision without acting (DQ-31) |
| `cleanup` | `true` | then run `ducklake_cleanup_old_files` |
| `grace` | the lake's `delete_older_than`, else 2 days | files scheduled for deletion longer ago than this are removed |
| `include_other` | `false` | D2 step 5 |

It is a table function. It returns one row per import snapshot: `snapshot_id`, `generation`,
`backup_type`, `backup_finish_date`, `backup_set_guid`, `action` (keep / expire / already expired), and
`reason` (for example "generation 3 outside the last 4", "last 14", "full of generation 6"). Rows for
other snapshots appear only with `include_other`.

**Execution.** The function computes the version list in its bind, from `imports` and `ducklake_snapshots`.
Then, unless it is a dry run, it runs two statements on a side connection:

1. `CALL ducklake_expire_snapshots('lake', versions := [ … ])`
   (`DL/functions/ducklake_expire_snapshots.cpp:103-112`);
2. `CALL ducklake_cleanup_old_files('lake', older_than := now() - grace)`
   (`DL/functions/ducklake_cleanup_files.cpp:141-153`).

These are separate transactions on DuckLake's public functions, the way DuckLake's own `CHECKPOINT` runs
them through a side `Connection` (`DL/storage/ducklake_checkpoint.cpp:15-30`). Neither creates a snapshot.
`ducklake_expire_snapshots` deletes the snapshot rows, and DuckLake schedules the files only they
referenced. The cleanup removes files scheduled longer ago than the grace. That includes files scheduled by
earlier runs, so space frees one run later than the expiry.

The function refuses to run inside an explicit transaction. Its effect would escape the caller's ROLLBACK,
the same reason DuckLake's compaction releases its metadata transaction only in autocommit
(`DL/functions/ducklake_compaction_functions.cpp:79`).

**What expiring a generation frees** (each needs no other snapshot to reference it):

- data files replaced by later imports;
- superseded cumulative delete files;
- page-map and mirror files of replaced runs;
- `pending_pages` images that were resolved, which is how uncommitted rows leave the lake for good
  (DQ-09);
- the files the importer's compaction replaced (D5).

### D4. The automatic policy

```sql
CALL ducklake_mssql_backup_configure('lake', keep_fulls := 4, keep_snapshots := 14, auto_expire := true);
```

This writes `retention.keep_fulls`, `retention.keep_snapshots`, `retention.auto_expire` and
`retention.grace` into `__mssql_backup.config`. The write is a native write, so it makes a non-import
snapshot, which D2 does not count.

With `auto_expire` on, every successful import registers a `ClientContextState`. Its `TransactionCommit`
callback fires only after the commit succeeded (`DD/src/transaction/transaction_context.cpp:81-83`) and
runs `CALL ducklake_mssql_backup_expire('lake')` with the stored policy on a side connection:

- **It is not inside the import's transaction.** DuckLake's retry loop rolls back the metadata transaction
  between attempts (`DL/storage/ducklake_transaction_state.cpp:2697-2703`). Snapshot deletes executed in
  that transaction would be lost on a retry while being reported as done.
- **Retries.** If the expiry fails, the import stays committed, and a `WARNING` goes to DuckDB's log (type
  `mssql_ducklake`). The next import runs the policy again; nothing is lost but time.
- **Inside an explicit transaction**, an import's hook fires at the user's COMMIT.

### D5. Retention and compaction

The importer owns compaction (022 D6): imported tables are created with `auto_compact = false`, and
`ducklake_mssql_backup_compact` runs DuckLake's merge and rewrite per table, then rebuilds the page map.

- **Order: compact, then expire, then clean up.** A compaction makes new files at snapshot C. The files it
  replaced stay referenced by the snapshots before C, and are freed only when those expire. Expiring first
  frees nothing of the small files a compaction is about to replace.
  `ducklake_mssql_backup_maintain(lake)` runs the three in that order, each as its own transaction.
  `auto_maintain` in config does so after an import instead of `auto_expire`.
- **When to compact.** When a table has more than *k* files below `target_file_size / 8` (the small files
  differentials leave), or a file has more than `rewrite_delete_threshold` of its rows deleted (the hot
  files whose cumulative delete files keep growing, 023 §D11). Both are config keys. Defaults are to be set
  from the bench (task 6).
- **Compaction snapshots are not counted** (D2). Keeping them costs almost nothing: they reference the new
  files, which later imports reference too.
- **The page map after a compaction** is stale until rebuilt (022 D6). Every import checks it first (023
  step 0), so a maintenance run between imports is safe.
- **DuckLake's own `expire_older_than`.** If a lake under a backup policy also has it set, `CHECKPOINT`
  expires by age underneath the policy. Every import warns when both are set, and the docs recommend
  leaving it unset, or above the policy's horizon. CHECKPOINT's compaction skips imported tables, and its
  cleanup only removes files past the grace, so neither does harm.
- **`ducklake_delete_orphaned_files`** after the grace also removes an unfinished import run's files (022
  D9). The maintenance run never calls it while a run directory is in state `exporting`.

### D6. Time travel by backup time

```sql
SET VARIABLE v = (SELECT snapshot_id FROM ducklake_mssql_backup_snapshot_at('lake', TIMESTAMPTZ '2026-10-01 00:00:00+00'));
SELECT * FROM lake.dbo.orders AT (VERSION => getvariable('v'));
```

`ducklake_mssql_backup_snapshot_at(lake, ts [, by := 'finish' | 'start'])` returns the latest import whose
`backup_finish_date` (or start date) is ≤ ts. The columns are `snapshot_id`, `backup_set_guid`,
`backup_type`, `backup_finish_date`, `generation`, `consistency`, and `expired`. When that snapshot has
expired, `snapshot_id` is NULL, `expired` is true, and `nearest_kept_before` / `nearest_kept_after` name
the alternatives. The helper never answers with another snapshot silently. Whether `AT (VERSION =>
getvariable(…))` binds is to be verified. If it does not, the docs show the literal form.

`ducklake_mssql_backup_history(lake)` lists every import: `import_seq`, `snapshot_id`, `generation`, type,
GUIDs, dates, LSNs, `consistency`, `expired`, and the file. Expiry is computed by an anti-join of `imports`
against `ducklake_snapshots`.

**What a snapshot shows**, stated in the docs:

- **With 090**: the database at the backup's LastLSN, with every committed transaction redone and the
  transactions open at LastLSN held back.
- **Tables marked stale** at that import (a loaded differential, 023 D7) are older for the skipped work,
  but clean.
- **Before 090** (quiet backups only), it is the CheckpointLSN state.
- `AT (TIMESTAMP => …)` is still available and means the import's commit time.

## Enforcement & security

- **Expiry is irreversible.** The dry run lists every decision with its reason. Both counts must be ≥ 1 or
  NULL, and 0 is refused. The latest import snapshot and the lake's latest snapshot are never expired.
- **Only import snapshots are expired**, identified by `imports`, not by text in `extra_info`. A user's
  snapshot is untouched unless `include_other` is set.
- **Concurrency with an import.**
  - An import plans at S0 and reads the lake `AT (VERSION => S0)` during its export (023 D3). S0 is either
    the latest import snapshot (always kept) or a later non-import snapshot (kept unless `include_other`).
    So an expiry running alongside an import cannot remove it.
  - With `include_other`, the expiry skips snapshots at or after the oldest S0 of any run directory in
    state `exporting`.
  - The cleanup's grace protects files that are still being read.
- **Data removal.** Pending images, side tables and replaced files leave the object store only through
  DuckLake's cleanup, after the grace. The docs say how long uncommitted rows in pending images can
  persist: until their snapshot expires, plus the grace.

## Testing

- **The policy matrix**, over a synthetic history (fresh F1, then D, D, F2 incremental, D, copy-only F, D,
  D, F3, D) with user commits and compactions between the imports:
  - m and n from 1 to unlimited;
  - the expected keep and expire sets asserted from the dry run, then after acting;
  - user and compaction snapshots untouched;
  - the latest snapshot kept.
- **Time travel.** At every kept snapshot, the oracle (022) equals RESTORE of that chain prefix. At an
  expired one, DuckLake's error, and the helper's `expired` row with its nearest neighbours.
- **Files.** The files referenced only by expired snapshots are scheduled, still present within the grace,
  and gone after it (`ducklake_cleanup_old_files` with a short grace in the test). The lake still equals
  RESTORE at the current snapshot. A differential applied after an expiry still matches.
- **The automatic policy:**
  - it runs after a successful import and not after a failed one;
  - a failure injected into the hook leaves the import committed and logs a warning;
  - the next import catches up;
  - inside an explicit transaction it fires at COMMIT.
- **Compaction interplay**:
  - `maintain` frees the replaced small files at the next run;
  - the page map is rebuilt and a following differential matches RESTORE;
  - `include_other := false` leaves the compaction snapshots;
  - with `expire_older_than` set, the import warns.
- **Concurrency**: an expiry with `include_other` during an import's export phase does not remove S0, and
  the import commits.

## Alternatives considered

- **DuckLake's `expire_older_than` alone**: by age, blind to generations, and it counts user snapshots.
  It cannot say "the last four fulls".
- **Expiry inside the import's transaction**: atomic in principle, but DuckLake's retry loop rolls the
  metadata transaction back between attempts (D4), so an expiry could silently not happen.
- **Retention per table**: DuckLake's snapshots are lake-wide, so there is nothing per table to expire.
- **One lake per generation**: the lake is the base, so a new full's lake would need the full imported
  fresh, which defeats incremental fulls. Time travel across generations would also be lost.
- **Implementing expiry against DuckLake's internals** (`DeleteSnapshots`, `GetFilesForCleanup`): it works
  and needs no side connection, but it couples to internals that DuckLake's public functions already wrap.

## Open questions for the owner

All are in [decisions.md](https://github.com/hugr-lab/mssql-extension/blob/main/specs/085-backup-reader-core/decisions.md), merged with the mssql series and ordered by impact:

| # | question | DECISIONS |
| --- | --- | --- |
| 1 | what *n* counts: over all kept generations (as drafted) or per generation | DQ-30 |
| 2 | do copy-only fulls start a generation (here yes; 023 C5 said no) | DQ-16 |
| 3 | `dry_run`'s default | DQ-31 |
| 4 | `include_other` and the importer's own compaction snapshots | DQ-32 |
| 5 | `auto_maintain` by default, or only expiry | DQ-33 |

## Follow-ups

- **Archiving before expiry**: export a generation's full snapshot to another lake or to Parquet before it
  expires.
- **DuckLake tags on generation snapshots**, if DuckLake grows snapshot tags.
- **A retention report**: bytes freed per generation, from the files scheduled for deletion.

## Tasks

| # | task | estimate |
| --- | --- | ---: |
| 1 | The generation number in 022's `imports` row; `ducklake_mssql_backup_history` with the expiry anti-join | 2 d |
| 2 | The policy computation (D2) and `ducklake_mssql_backup_expire`: dry run, the side-connection calls, cleanup, the refusals | 3 d |
| 3 | `ducklake_mssql_backup_configure`, the config keys, the post-commit hook for `auto_expire` / `auto_maintain`, warnings | 3 d |
| 4 | `ducklake_mssql_backup_snapshot_at`, and checking that `AT (VERSION => getvariable(…))` binds | 1 d |
| 5 | `ducklake_mssql_backup_maintain`, the `expire_older_than` warning, the run-directory guard | 2 d |
| 6 | Tests: the policy matrix, time travel at kept and expired snapshots, files after the grace, the auto policy and its failure, compaction interplay, concurrency; compaction thresholds from the bench | 5 d |
| 7 | Docs page | 1 d |
| | **total** | **17 d ≈ 3.5 wk** (2–3.5 with overlap) |
