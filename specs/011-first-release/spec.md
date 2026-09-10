# Spec 011: the first release — v0.1.0 on the v1.5.5 line, through community-extensions

- **Status**: draft
- **Date**: 2026-09-10
- **Author**: VGSML

## Summary

The extension builds green on every platform, has 163 server-backed assertions on both commit paths
and a concurrency regression test, and has never been tagged. This spec is the list of what stands
between that and a build a user can `INSTALL mssql_ducklake FROM community`: a version and a tag,
the community descriptor, honest documentation of what is and is not done, and the spec statuses
brought in line with the code. The release line is **v1.5.5** — the 2.0 line has no released tag
in any of the three pinned components (design 002 §5).

## Problem

Nothing here is a feature; each is something a release cannot ship without:

- **No tag, ever.** `distribution.yml` triggers on `v*` and has only ever run on pushes to `main`.
- **No community descriptor.** The distribution channel (CLAUDE.md) is a PR to
  `duckdb/community-extensions` carrying `extensions/mssql_ducklake/description.yml`; nothing in
  this repository prepares it.
- **Spec statuses lag the code.** specs/004 is "accepted (in progress)" with a performance target
  the benchmark still misses (design 002 §1: 2.32x total against postgres); specs/005 is `draft`
  with phase 2 shipped behind `MSSQL_DUCKLAKE_SERVER_COMMIT`. A user reading the specs cannot tell
  what the release contains.
- **Known defects are scattered across follow-up sections**, not stated in one place a user reads.
- **The test server is not what the README recommends.** `docker/init/sqlserver.sql` creates
  `lake_meta` with the image's default `SQL_Latin1_General_CP1_CI_AS`; the manager converts every
  catalog column to the UTF-8 BIN2 collation (specs/006 D4), so this works — but the suite has never
  run against a database *created* UTF-8, which is what the README tells users to do.

## Design

**Version.** `v0.1.0`. The extension is experimental; the version says so, and the pins say the
rest. Where it lives: the `extension_config.cmake` / `CMakeLists.txt` version string and the tag.
Bumping it is part of the release commit, not a separate one.

**Tag → distribution build.** Tagging `v0.1.0` on `main` runs `distribution.yml` — Linux amd64 and
arm64, macOS amd64 and arm64, Windows MSVC and MinGW, wasm deliberately excluded — and its artifacts
are the release. The GitHub release is created from the tag with the notes below.

**Community descriptor.** `extensions/mssql_ducklake/description.yml` in a fork of
`duckdb/community-extensions`, with:

- `name: mssql_ducklake`, the repo and the `v0.1.0` ref, `duckdb_version: v1.5.5`;
- `excluded_platforms: wasm_mvp;wasm_eh;wasm_threads` — mssql is raw TDS sockets (CLAUDE.md);
- `requires_toolchains` as the mssql extension declares (openssl through vcpkg);
- a description that says what the thing is: *embeds ducklake at `<pin>`; mutually exclusive with
  the stock ducklake extension; requires the mssql extension* — design 001 §8 called the honest
  wording a requirement, not a nicety, because a reviewer will ask.

The PR to community-extensions goes up after the tag, and this spec's status flips to
`implemented` when it merges, not before.

**Documentation a user reads.** `README.md` gains a section that states, in this order:

1. *How to install and attach* — `INSTALL mssql FROM community; LOAD mssql; INSTALL mssql_ducklake
   FROM community; LOAD mssql_ducklake;` then `ATTACH 'ducklake:mssql:…'`; the load order and the
   autoload trap (CLAUDE.md).
2. *What it needs from the server* — SQL Server 2019+ or Azure SQL (the UTF-8 collation gate,
   specs/004 D3); the login's permissions (creates tables and indexes in the metadata schema);
   `sys.databases` visibility is not required.
3. *Known limitations*, stated as limitations:
   - phase 2's server-side commit is off by default and experimental
     (`MSSQL_DUCKLAKE_SERVER_COMMIT`);
   - a retried commit can leave an unregistered, empty `ducklake_inlined_data_*` table behind
     (specs/004 follow-ups); a flush leaves empty inlined tables (specs/005 D10, upstream);
   - the type matrix: FLOAT NaN, TIMESTAMP_NS, HUGEINT and nested types are stored as text in
     inlined tables (specs/004 D4);
   - performance against the postgres backend by workload, with the numbers from design 002 §1 —
     not "fast", the ratios;
   - concurrent writers: fixed in specs/007; the systemic fix (SNAPSHOT isolation) is
     hugr-lab/mssql-extension#331.
4. *Versions* — the pin table, and that a ducklake fix reaches users only with this extension's
   next release (the vendoring rule).

**Spec statuses.** specs/004 → `implemented (target missed, see Measured)`, already what its
Measured section says. specs/005 → `implemented (phase 2 behind a switch)`, with one paragraph
naming the switch and why it is off. The README index follows.

**The test server, created UTF-8.** `docker/init/sqlserver.sql` creates `lake_meta` `COLLATE
Latin1_General_100_BIN2_UTF8`; the suite then exercises the configuration the README recommends.
The manager's conversion sweep (specs/006 D4) still runs — a user with a CI_AS database is the
common case and stays tested by the collation assertions in the integration suite, which create
their own state.

**Release notes** are the specs: one line per spec 002 – 010 with its one-sentence summary, the
pin table, and the limitations list above verbatim.

## Enforcement & security

The two load-time gates are what a user meets first and both are documented in the README section
above: the extension refuses to load beside the stock ducklake extension, and refuses to load
without the mssql extension (`test/sql/deps_gate.test`). Neither is new; both are now written where
a user will read them.

## Testing

- The distribution build on the tag, all platforms green — the same run that has passed on every
  push to `main` since specs/006.
- The community-extensions build: their CI builds the descriptor's ref on their runners; green there
  is the proof that the descriptor is right (`excluded_platforms`, toolchains, the vcpkg manifest).
- `scripts/ci/smoke_load.sh` against the *installed* artifact — `INSTALL mssql_ducklake FROM
  community` on a stock DuckDB v1.5.5 CLI — once the community build is published: the out-of-tree
  load, both gates, one local-file lake cycle.
- The integration suite against a UTF-8-created `lake_meta`, before the docker init change merges.

## Alternatives considered

- **Release on the 2.0 line.** There is no released tag of duckdb 2.0, mssql on 2.0 or ducklake
  for 2.0; community-extensions builds against released DuckDB. Not a choice yet.
- **Wait for the performance target.** specs/004's "≥ postgres" is missed by 2.32x at a thousand
  tables (design 002 §1), and the two phases before this one (specs/008, 009) address the hot
  path. But the extension is correct, tested and experimental; a release with the ratios published
  is more useful to the people who will report the next defect than a release withheld until a
  number moves.
- **Ship phase 2 on by default.** It is correct for the commits it accepts (specs/005 D4) and
  incomplete for the rest; off by default with a documented switch is the honest shape.

## Follow-ups

- The upstream track from CLAUDE.md — contributing the manager in-tree to ducklake — is taken up
  if the release finds users.
- After the 2.0 line is released across all three pins: design 002 §5 and §9 phase E.
