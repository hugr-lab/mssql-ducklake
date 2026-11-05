> The reader these specs build on, and the research and owner decisions behind both series, live in
> mssql-extension: [spec series 085–090](https://github.com/hugr-lab/mssql-extension/blob/main/specs/085-backup-reader-core/series.md), [decisions](https://github.com/hugr-lab/mssql-extension/blob/main/specs/085-backup-reader-core/decisions.md),
> [research](https://github.com/hugr-lab/mssql-extension/blob/main/specs/085-backup-reader-core/research/README.md) (links resolve once hugr-lab/mssql-extension#526 merges).

# mssql-ducklake: importing SQL Server backups into DuckLake (draft series)

Drafts, 2026-11-05. They are not in the mssql-ducklake repository yet. Each one follows that repo's
`specs/TEMPLATE.md`, and the folders mirror its `specs/NNN-slug/spec.md` layout so they can be moved
in unchanged. **The numbers are tentative.** They continue after 020, the highest number in
`mssql-ducklake/specs/` today, and that repository is active, so they may shift before they land.

Sources: [RESEARCH.md](https://github.com/hugr-lab/mssql-extension/blob/main/specs/085-backup-reader-core/research/RESEARCH.md) (the owner's decisions; authoritative), [research-ducklake-import.md](https://github.com/hugr-lab/mssql-extension/blob/main/specs/085-backup-reader-core/research/research-ducklake-import.md)
(the pipeline, with file:line references into DuckLake), [research-architecture.md](https://github.com/hugr-lab/mssql-extension/blob/main/specs/085-backup-reader-core/research/research-architecture.md), and the
experiments E1, E2, E5, E6 and E7 (partitioning in a backup; it finished after the first draft and is
integrated: the proposal rule, the ownership rule for SWITCH, the maintenance evidence). The owner's open
questions of both series are merged into one list, [decisions.md](https://github.com/hugr-lab/mssql-extension/blob/main/specs/085-backup-reader-core/decisions.md) (DQ-nn).

## The series

| spec | title | what it delivers | estimate |
| --- | --- | --- | ---: |
| [022](spec.md) | importing a full backup | a full `.bak` becomes one DuckLake snapshot: the pipeline (export first, one registering commit), parallel writers, the `__mssql_sys` mirror and allocation images, provenance, the partition and sort proposal at creation (then DuckLake's layout), inlining off, DuckLake's standard compaction, retries and orphans, the SQL surface, progress | 6–8 wk |
| [023](../023-incremental-import/spec.md) | applying a file on top of the lake | differentials and later fulls applied with only the lake and the one file: chain validation, DuckLake `DELETE`s by `_mssql_page` in the table the base allocation images name, the LOB fragment map, schema evolution inside the commit, pending and stale pages, partition maintenance with E7's ownership rule (SWITCH by lake-side copy), columnstore deletes by rowgroup key | 10–11 wk |
| [024](../024-backup-retention-and-time-travel/spec.md) | retention and time travel | generations, `ducklake_mssql_backup_expire`, the automatic policy, the backup time → snapshot helper, and how retention interacts with DuckLake's standard compaction | 2–3 wk |

**Total: about 18–23 weeks of mssql-ducklake work** (022 41 d, 023 56 d, 024 16 d), gated on the mssql
reader series (below; about 259 d of its own, ≈ 52 weeks for one developer, ≈ 34 with two). This is far
above RESEARCH §8's phase 2 estimate ("3–4 wk"), which predates the owner's decisions. The decisions that
added the work are the Parquet-first registration, the pending and stale lifecycle, schema evolution
with rewrites, partition derivation and maintenance, columnstore deletes, retention, and an oracle
harness that runs the E1 and E6 scenarios as tests.

### Why this split

The owner's suggested split is kept, with one change: views are not a spec.

- **022 and 023 stay apart**, although they share one pipeline. 022 is what a lake is born with: its tables,
  service columns, side tables, partitioning and sort, and the registering commit. 023 is the
  apply algorithm, which reads all of that back. 023 depends on the incremental API (mssql 089) and, for
  loaded files, on tail recovery (090). 022 does not, so it can ship first.
- **Pending and stale pages stay in 023**, as the owner placed them, and are not a spec of their own. They
  slot into the middle of the apply order: pending images are resolved before the file's own redo, and
  stale pages are forced into the changed set. A separate spec would have to describe the apply twice.
  023's task list gives them their own phase, gated on 090. The pending table is *created* by 022, since a
  loaded full already produces pending pages.
- **Views from backups are a follow-up, not a spec.** Capturing the T-SQL text costs a few rows per import
  and needs no decision, so 022 stores it in `__mssql_backup.views` and 023 versions it. Translating
  T-SQL into DuckDB views has no owner decision yet: which subset, and whether it is opt-in per view. It
  does not affect import correctness. The stored text means it can be done later without re-reading old
  backups. See 022 *Follow-ups*.

## What the series consumes from mssql (085–090)

Draft names, as given by the owner: **085** core, **086** row formats and types, **087** encryption,
**088** columnstore, **089** incremental API, **090** tail recovery ("backup-tail-recovery"). The import
logic stays in mssql-ducklake and the format logic stays in mssql, the same split mssql-ducklake already
has with `mssql_exec` / `mssql_scan`. Nothing in mssql knows about the lake: the lake reaches the reader
only as side inputs, which are plain relations or Parquet paths.

| mssql surface (names tentative) | spec | used by | for |
| --- | --- | --- | --- |
| `mssql_backup_info(paths, position)` | 085 | 022, 023, 024 | header fields: kind, copy-only, GUIDs, the four LSNs, family and recovery-fork ids (the fork id still to locate), dates, family/stripes, compression, encryption thumbprint |
| `mssql_backup_tables(file [, base])`, `mssql_backup_views(file)` | 085 / 089 | 022, 023 | the DDL plan, the `partitioning` a spec is proposed from (E7); view text; with `base`, a lone differential's tables |
| `mssql_backup_catalog(file [, base_table] [, since_lsn, base])` | 085 / 089 | 022, 023 | with no base table, the allow-listed base tables a mirror holds; with one, its raw rows for `__mssql_sys` |
| `mssql_backup_scan(file, schema, table [, since_lsn, base])` with `_mssql_rid`, `_mssql_page` (and 089's `_mssql_lob`) | 085 / 089 | 022 phase A, 023 fallback | rows (the v1 pipeline and the oracle) |
| `mssql_backup_chain_check(file, …)` | 089 | 023 | the chain rules C1, C3–C5 from the file alone; W and `reflected_lsn` to record |
| `mssql_backup_changed_pages(file [, schema, table], since_lsn, base [, expand])` | 089 (+090 kinds) | 023 | `changed`, `freed`, `dead_au`, `gone_au`, `au_moved` (E7's ownership rule), `dead_rowgroup`; with 090 `pending_*` and `stale`. Each row with `base_au_id` (the AU of its base IAM image), that AU's owner in the base mirror, `from_object_id` / `from_index_id` / `from_partition_number` (the table its rows are deleted from, or for `au_moved` move from), and `holds_rows BOOLEAN` (NULL without `base`; true when lake rows can be on the page: the delete set's and the SWITCH copy's data-pages-only filter); `expand := true` gives one row per page |
| `mssql_backup_allocation(file [, since_lsn, base])` | 089 | 022, 023 | the allocation images kept in `__mssql_sys.alloc_pages` (its lake-mode output is the images the importer keeps); their IAM images name the AU that owned each page, whose owner the reader reports as `from_*` and the registry maps to a lake table; its attribution check refuses a page the lake gets rows of that the images leave off its header AU's chain, on both paths (the export repeats it) |
| `mssql_backup_export_plan(file, …)`, `mssql_backup_export(file, …)` | 089 | 022 phase B, 023 | the parallel writer: data files in DuckLake's layout (`partition_by`, `order_by`, `target_file_size`, `parquet_options` from the lake). Side inputs under `base/`: the mirror, the allocation images, the old LOB values and maps, the pending set (089 D10); the stale set stays in the lake |
| tail recovery: `recovery`, `loaded_diff`, `reflected_lsn` on the scan, the export, `changed_pages` and `mssql_backup_allocation`; `mssql_backup_pending_pages` | 090 | 022 (loaded full), 023 | no dirt in the lake; the pending set kept between files; the allocation images of the recovered state, which `alloc_pages` keeps |
| the `mssql_backup_key` secret (scope first, then thumbprint) | 087 | all | an encrypted backup or TDE (TDE refused until E10, 087 D9); the import passes no key material |
| columnstore rows by rowgroup (`_mssql_page` = the rowgroup key), delete bitmap entries, rowgroup states | 088 | 022, 023 | CCI tables |
| the opened-file cache (per instance, keyed by path + size/ETag + BackupSetGUID) | 085 | all | the import calls the file functions many times, on side connections too; it also keeps a file's recovery (090) |

The exact contract the importer needs from 089 is stated in 022 §D4 and 023 §D2. 089's own draft is the
authority on names and shapes, and the two were reconciled with it on 2026-11-05 (option names
`position`, `loaded_diff`, `reflected_lsn`, `base`). The LOB map is the opaque `_mssql_lob` BLOB
(DQ-02, decided), with the raw old bytes only for values that do not re-encode exactly (DQ-01,
accepted: the hybrid).

## Order and phase map

```text
mssql, track A (critical path):  085 ──► 089 PR 1 ──► 090
mssql, track B (coverage):       085 ──► 086 PR 1-2 ──► 087 ──► 088 ──► 086 PR 3-6 ──► 089 PR 2

mssql-ducklake:  I1  022 phase A   needs 085 (+086 for types)
                 I2  024           after I1, beside I3
                 I3  023 core      needs 089 PR 1; 022 phase B needs 089 PR 2
                 I4  023 load      needs 090
                 I5  023 CCI       needs 088 (+ E11)
```

| phase | needs | scope | estimate |
| --- | --- | --- | ---: |
| **I1** | 085 (+086 for types) | 022 phase A: the SQL pipeline (`COPY` from `mssql_backup_scan`, `ducklake_add_data_files`), DDL, mirror and allocation images, provenance, the partition and sort proposal, the oracle harness, quiet fulls only (an inconsistent full is refused; no user option imports it). Without 089's `_mssql_lob` these lakes carry no LOB maps (DQ-05) | 4–5 wk |
| **I2** | I1 | 024: generations, expire, policy, snapshot helper. It can run alongside I3 | 2–3 wk |
| **I3** | 089 (PR 1; PR 2 for 022 phase B) | 022 phase B (C++ registration through DuckLake's transaction, the parallel export, manifests, resume) and 023's core for quiet chains: chain validation, deletes by `_mssql_page`, mirror and DDL, schema evolution, LOB map, SPLIT / MERGE / TRUNCATE / REBUILD PARTITION, and rowstore SWITCH by E7's ownership rule | 3.5 wk (022 phase B) + 8 wk (023 core), after 089's ≈ 8.5 wk of mssql work (PR 1 31.5 d, PR 2 9 d) |
| **I4** | 090 | 023's load phase: pending and stale tables and their lifecycle, skip-whole by default, the E6 load scenarios and counterfactuals | 2–3 wk |
| **I5** | 088 (and E11) | 023: columnstore deletes and rowgroup transitions; a columnstore SWITCH by rowset | 1 wk |

087 adds no import code: encrypted and TDE samples join the I1 and I3 test matrices (about 0.5 wk). A TDE
backup is refused by name until E10 reads its log tail (087 D9: its tables report `unknown`).

## Cross-cutting decisions these drafts rely on (owner, 2026-11-05)

- **The lake is the base.** A full is imported once. Every later file is applied on top WITHOUT reading the
  old full or any intermediate file. Each imported file is one DuckLake snapshot, so time travel is the
  backup history.
- **Parquet first.** Workers write the data files with no lake transaction open. Then ONE transaction
  re-validates the plan first, runs DuckLake's own `DELETE`s (and a SWITCH's copy) and registers every
  file through DuckLake's own transaction (`AppendFiles`); it holds the catalog's server transaction
  while the `DELETE` scans and the copy run, and a failure anywhere rolls it all back. The importer never
  writes metadata rows itself. `ducklake_add_data_files` is v1, the fallback and the oracle.
- **Deletes are DuckLake's `DELETE` by value**: per affected lake table, `DELETE … WHERE _mssql_page IN
  (…)`, run before the new files are registered; the table is the one the base allocation (IAM) images
  and the mirror name for the page. DuckLake finds the rows and writes its own delete files. Compaction is
  DuckLake's standard compaction: nothing the importer keeps refers to a file position.
- **Service columns** `_mssql_rid`, `_mssql_page` and the LOB fragment map. `__mssql_sys` mirrors the base
  tables and keeps the allocation images; `__mssql_backup.imports`, `.tables` and `.config` hold
  provenance and settings.
- **Follow DuckLake's rules, do not replicate SQL Server** (2026-11-05, DQ-10): SQL Server's partitioning
  and clustered key only propose a table's spec at creation, and manual DuckLake changes win; partition
  values are computed exactly as DuckLake computes them, in the importing session (its TimeZone for
  TIMESTAMPTZ, DuckLake's TIMESTAMP_NS rounding); files follow DuckLake's partition values, sort order and
  file size, never SQL Server's partitions (those are work units); a SWITCH is a lake-side copy plus a
  `DELETE` by `_mssql_page`, its rows found by E7's AU / rowset rule through the base IAM images
  (columnstore: a compressed rowgroup by the moved rowset id in its `0x02` rowgroup key; the delta-store
  and delete-bitmap rowsets move as AUs, `au_moved` with their base IAM pages, and the delta store's
  rows are found by `_mssql_page` in the `0x01` form).
- **Consistency:** no dirt in the lake (090); pending pages pass the allocation check first; stale pages
  are healed by the next file; a loaded differential skips whole the tail transactions that touch pages
  the file lacks; files are ordered by CheckpointLSN; the chain is validated.
- **Keys** come from mssql's `mssql_backup_key` secret, never inline.

## Open questions that cut across the series

The four cross-cutting questions are in [decisions.md](https://github.com/hugr-lab/mssql-extension/blob/main/specs/085-backup-reader-core/decisions.md) with the specs' own: the entry points (DQ-21),
the helper naming family (DQ-35), writes by users to imported tables (DQ-08), and uncommitted data in
pending page images (DQ-09). Each spec maps its own list there.
