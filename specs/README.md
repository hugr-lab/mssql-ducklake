# Feature specs

We keep one **lightweight spec per feature** here. This is deliberately *not* full spec-kit — no
plan/tasks/constitution machinery, no generated branches. Just a short, honest document per feature
so design decisions are written down and reviewable.

## Layout

Each feature is a numbered folder holding its `spec.md` (plus any feature-local assets):

```text
specs/
  TEMPLATE.md
  001-bridge-extension/
    spec.md
  002-<slug>/
    spec.md
```

## Process

1. **Write a spec first** (or alongside the work): create `specs/NNN-slug/spec.md` from
   `TEMPLATE.md`, where `NNN` is the next zero-padded number and `slug` is a short kebab-case name.
   Fill in the problem, the design, and how it will be tested.
2. **Implement with tests.** Prefer sqllogictest (`test/sql/`); add C++ tests only where SQL cannot
   express it.
3. **Keep the spec current.** When the design shifts during implementation, update the spec. Set
   `Status: implemented` when it lands; reference the spec in the commit/PR.
4. **Supersede, don't rewrite history.** If a later feature reverses a decision, add a new spec and
   mark the old one `Status: superseded by NNN`.

Deeper research and thinking-out-loud lives in a local, un-committed `design/` folder (gitignored) —
specs are the shareable distillation of that work.

## Index

| Spec | Title | Status |
| --- | --- | --- |
| [001](001-bridge-extension/spec.md) | the bridge extension — DuckLake metadata on SQL Server | superseded by 002 |
| [002](002-embedded-ducklake/spec.md) | the standalone extension — embedded ducklake + mssql manager | accepted |
| [003](003-mssql-extension-v0.2.5/spec.md) | mssql-extension v0.2.5 — what the manager needs from the runtime pair | implemented (mssql v0.2.5) |
| [004](004-manager-phase-1/spec.md) | the SQL Server metadata manager, phase 1 — parity with postgres | implemented (target missed, see Measured) |
| [005](005-server-side-commit/spec.md) | phase 2 — the commit as one call to the server | draft |
| [006](006-write-path/spec.md) | the write path — the appender, MERGE, and the catalog's own storage | implemented |
| [007](007-concurrent-commit/spec.md) | concurrent commits — the conflict check that read one table twice | implemented |
| [008](008-tsql-read-layer/spec.md) | the T-SQL read layer — one probe through `mssql_scan`, and why not five | implemented |
| [009](009-inlined-first-write/spec.md) | the first inlined write is a plan compile per table | implemented (reconnaissance; mssql-extension#334) |
| [010](010-manager-split/spec.md) | splitting the manager, and two documents that say the wrong thing | implemented |
| [011](011-first-release/spec.md) | the first release — v0.1.0 on the v1.5.5 line, through community-extensions | draft |
| [012](012-forced-parameterization/spec.md) | forced parameterization on the catalog's database | implemented |
