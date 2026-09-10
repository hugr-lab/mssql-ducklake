# Spec 011: the first release — v0.1.0 on the v1.5.5 line

- **Status**: implemented (v0.1.0 tagged 2026-09-10; duckdb/community-extensions#2683)
- **Date**: 2026-09-10
- **Author**: VGSML

## Summary

The extension builds green on every platform, has 289 server-backed assertions on both commit
paths, a concurrency regression test, a documentation site, and a benchmark it can publish — and
has never been tagged. This spec is the machinery that turns a tag into a release and keeps the
site's version in step with it, and the record a release cannot ship without: the version, the
community descriptor, the release notes, the spec statuses. The release line is **v1.5.5** — no
released tag of duckdb 2.0, mssql on 2.0 or ducklake for 2.0 exists (design 002 §5).

## Problem

- **No tag, ever.** `distribution.yml` triggered on `v*` and built the binaries; nothing published
  them, and nothing checked that the tag said what the code says.
- **The version was a build hash.** `mssql_ducklake_version()` answers a constant since specs/013
  (`0.1.0-dev`), but no step ties the constant to a tag.
- **The docs site had a versioning contract and no automation**: "at each release run
  `docs:version <X.Y.Z>` and commit the snapshot" — a step to remember, a snapshot to keep in git.
- **No community descriptor**, no release notes, and spec statuses that lag the code (specs/004
  "accepted (in progress)", specs/005 `draft`).

## Design

**D1 — the tag is the release.** `.github/workflows/release.yml` runs on a `v*` tag, in three
jobs. `guard` reads `MSSQL_DUCKLAKE_VERSION` from `CMakeLists.txt` and `version`/`ref` from
`description.yml` and fails if any disagrees with the tag — a release whose binary would report
another version than its tag cannot be built. `build` is the same reusable workflow
`distribution.yml` runs on `main` (`_extension_distribution.yml@v1.5.5`, wasm excluded), so the
release binaries are the ones the community-extensions build would produce. `release` renames each
platform's artifact to `mssql_ducklake-<version>-<platform>.duckdb_extension`, writes
`SHA256SUMS.txt`, and publishes the GitHub release with them and a body that names the pins, the
docs, the release notes and the install lines; a tag with a `-` in it publishes as a pre-release.
`distribution.yml` no longer runs on tags: one build per tag.

**D2 — the site's version follows the tags, automatically.** `pages.yml` checks out with the tags
and, before building, snapshots every release tag's `website/docs` and `sidebars.ts` as that
version: the tag's tree is swapped in, `docusaurus docs:version <X.Y.Z>` runs on it, the live tree
is swapped back. `versions.json`, `versioned_docs/` and `versioned_sidebars/` are generated and
gitignored — nothing is committed for a release's docs, the tag *is* the snapshot. The latest
release serves at the root, the live docs as *Next* with the unreleased banner, and a published
release triggers the deploy (`release: published`), so the site changes the moment the release
exists. Verified locally with a temporary tag: the snapshot, the dropdown, the build.

**D2b — what v0.1.0 taught.** The release's own event did not deploy the site: a GitHub
release created with the workflow's `GITHUB_TOKEN` is an event made by that token, and events
made by it start no workflows — `release: published` never fired. The push of the tag is the
user's event and does; `pages.yml` now runs on `v*` tags (and on demand), and the release trigger
is gone. The v0.1.0 deploy was dispatched by hand.

**D3 — the version.** `MSSQL_DUCKLAKE_VERSION` is `0.1.0` in this commit; `description.yml` says
`0.1.0` and `ref: v0.1.0`; the guard holds them together. After the release the constant goes to
the next version with `-dev`, and the descriptor stays at the released tag until the next release
(that is what community-extensions builds). `duckdb_extensions().extension_version` keeps the
build's git hash.

**D4 — the community descriptor.** `description.yml` at the root, copied to
`duckdb/community-extensions/extensions/mssql_ducklake/description.yml` by the release's PR there
with `ref` replaced by the tag's commit SHA (`git rev-list -n 1 vX.Y.Z`): every descriptor there
names a commit, and the repo's own copy cannot — a commit does not know its own SHA — so it names
the tag and the guard checks the tag. The text is Markdown, one paragraph per line (the site joins
wrapped lines, and a line starting `- ` becomes a list item). The descriptor carries: name, an
honest description (embeds DuckLake at a pin, mutually exclusive with stock ducklake,
needs the mssql extension), `excluded_platforms` = the mssql extension's set plus wasm (the
manager is nothing where mssql is not), `test_config` skipping the server-backed suite, a
hello-world attach. Their build runs our `extension_config.cmake`, which builds the mssql
extension beside ours for the tests, as the distribution build does. The PR goes up after the tag
exists, and its green build is the proof the descriptor is right.

**D5 — release notes and statuses.** `website/docs/releases.md` — a page per release, versioned
with the site — carries what v0.1.0 does, what the manager adds, the numbers, what is not there,
the platforms; the GitHub release body links to it. specs/004 is `implemented` with its target
reached later (specs/012, 014: 1.54x with commits at parity); specs/005 is `implemented (phase 2
behind a switch)`. The README carries the badges the mssql extension's does.

**D6 — the test server stays on the legacy collation.** The draft wanted `lake_meta` created
UTF-8, "what the README recommends". The docs recommend no such thing: the manager collates every
catalog column explicitly (specs/006 D4), and a `CI_AS` database — the common case — is the harder
one to get right. The suite keeps it.

## Enforcement & security

- `release.yml` has `contents: write` in its last job only; the build is the reusable workflow
  with its own permissions; the guard runs first and fails closed.
- The site's snapshot step reads tags only; a tag without `website/docs` is skipped, not failed.
- The two load-time gates a user meets first — stock ducklake loaded, mssql missing — are on the
  site's troubleshooting page.

## Testing

- The guard, negated: a tag that disagrees with the constant fails in `guard` before any build.
- The snapshot step on a temporary local tag: `docs:version` on the tag's tree, the version in
  `versions.json`, the site building with the dropdown.
- `mssql_ducklake_version()` answers `0.1.0` (the unit test pins the shape).
- After the tag: the distribution build inside `release.yml` green on every platform, the release
  page with six binaries and their checksums, the site serving `/0.1.0/`… at the root with the live
  tree at `/next/`; the community-extensions build of the descriptor's ref green.

## Alternatives considered

- **Committed docs snapshots** (the mssql extension's way): a step to remember at each release and
  a copy of the docs in git per version. The tags already hold every version of the docs; deriving
  the snapshots from them at deploy time removes both.
- **The version from the tag at build time** (`git describe` in CMake): depends on the tags being
  fetched wherever the extension is built — the community build checks out a ref, not necessarily
  with tags. A constant the guard checks is one line and always right.
- **Release on the 2.0 line**: nothing released to build against; not a choice yet.
- **Wait for the performance target**: it is reached where it matters (commits at parity); the
  rest is documented with its numbers.

## Follow-ups

- After the tag: the PR to duckdb/community-extensions; `INSTALL mssql_ducklake FROM community`
  smoke-tested on a stock v1.5.5 CLI once their build publishes; the constant to `0.2.0-dev`.
- The upstream track — contributing the manager in-tree to ducklake — if the release finds users.
- The 2.0 line, when all three pins have a release (design 002 §9 phase E).
