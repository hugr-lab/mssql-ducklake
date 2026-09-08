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
| [001](001-bridge-extension/spec.md) | the bridge extension — DuckLake metadata on SQL Server | accepted |
