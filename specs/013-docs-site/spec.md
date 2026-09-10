# Spec 013: the documentation site

- **Status**: implemented
- **Date**: 2026-09-10
- **Author**: VGSML

## Summary

Before the first release the extension gets user documentation the way the mssql extension has it:
a Docusaurus site in `website/`, versioned per release, published to
https://hugr-lab.github.io/mssql-ducklake/ by GitHub Pages, and linked from the org site. The
README shrinks to what a README is for and points at the site; CLAUDE.md gains the rules the site
brings with it.

## Problem

Everything a user needs to know existed — in CLAUDE.md, in the README, in twelve specs — and none
of it was written for a user. The README's status paragraph still described the state before the
manager existed ("every later write to that table fails … the manager is being implemented"), the
attach forms, the catalog's requirements and the settings were scattered, and the performance
numbers lived in a gitignored research note. The org site lists the mssql extension's docs in its
footer and its FAQ; this extension was not there.

## Design

**D1 — the same site, the same contract.** `website/` mirrors `hugr-lab/mssql-extension/website`:
Docusaurus 3.10 classic with the mermaid theme, the org's CSS and logo, docs at the site root,
`onBrokenLinks: 'throw'`, the version dropdown, and the versioning contract in the config: at each
release `docs:version <X.Y.Z>` snapshots the docs, the snapshot serves at the root, the live tree as
*Next*. Until v0.1.0 (specs/011) is snapshotted, the current docs are the only version.

**D2 — what the pages say.** Thirteen pages, each sourced from a spec or from CLAUDE.md, none from
memory:

| page | from |
| --- | --- |
| Overview, Getting Started | specs/002, 003, the README |
| The catalog in SQL Server: Attaching, Shaping, Requirements | specs/003, 004 D3, 006 D4, 008, 012 |
| Writing (the commit path, inlining and types, concurrent writers, the server-side commit) | specs/004 D4, 005, 006, 007, 009 |
| Performance | specs/012's bench table, 008, 009 |
| Settings, Limitations, Troubleshooting | the settings and switches in the code; the follow-up sections of every spec |
| Versions, Development | CLAUDE.md |

The platform matrix on the Requirements page says what was exercised (SQL Server 2025 in the
suite) and what is expected from the documentation (Azure SQL Database, Managed Instance, Fabric SQL
database), and names what cannot hold a catalog (Fabric Warehouse, Synapse dedicated pools).

**D3 — publishing.** `pages.yml` builds `website/` and deploys it on a push to `main` touching it,
on a published release, and on demand; `docs-build.yml` builds it on pull requests, after
`scripts/ci/check_docs_links.py` — both copied from the mssql extension with their reasoning. GitHub
Pages on the repository is set to the workflow source.

**D4 — the org site.** hugr-lab.github.io gains the link where the mssql extension has its: the
footer's Community column, the FAQ's data-lakes answer, and the overview's DuckLake bullet.

**D5 — the README and CLAUDE.md.** The README keeps the one-paragraph description, the install
block, the exclusivity warning, an honest status, the pins, the build commands, and points at the
site for the rest; the stale status paragraph is gone. CLAUDE.md lists `website/` in the structure
and the build in the commands, and states the two rules: relative links only, and the release-time
snapshot.

## Enforcement & security

Nothing runs against a server. The Pages deploy has `pages: write` and `id-token: write` and runs
only from `main`; the PR gate has `contents: read`. No secrets.

## Testing

- `npx docusaurus build` with `onBrokenLinks: 'throw'` and `onBrokenMarkdownLinks: 'throw'`: every
  link resolves, including anchors between pages.
- `scripts/ci/check_docs_links.py`: no absolute internal links.
- Every claim on the pages traces to a spec section or a code path named in D2; the numbers are
  the ones in specs/012.

## Alternatives considered

- **Docs inside the org site** (a section under hugr-lab.github.io/docs). One repository would then
  version two products on its own schedule; the mssql extension chose a site per repository for that
  reason, and this one follows.
- **A `docs/` folder of markdown without a site.** Not versioned, not linked, not what the org
  does.

## Follow-ups

- specs/011 snapshots `0.1.0` at the release and links the release notes from the site.
- Pages for what is not yet there: the server-side commit once it ships, Azure SQL Database once the
  suite runs against it.
