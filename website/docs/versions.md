---
title: Versions
sidebar_position: 6
---

# Versions and compatibility

One release line, bumped together in one commit:

| piece | pin | role |
| --- | --- | --- |
| DuckDB | **v1.5.5** | the release line the extension is built and distributed for |
| DuckLake | branch `v1.5-variegata` (embedded, at a named commit) | compiled into the extension, unmodified |
| mssql extension | **v0.2.5** or newer at runtime | the runtime pair: `mssql_exec`/`mssql_scan`, the catalog attach, the TDS codecs |

**The embedded DuckLake bumps on this repository's schedule**, and a DuckLake fix reaches users
only with this extension's next release; every release names its DuckLake pin. Bumps are kept
cheap and frequent for that reason.

**mssql v0.2.5** is the minimum because it is the version that answers `dbo` as the catalog's
default schema; older ones answer DuckDB's `main` and the attach fails. `SELECT mssql_version();`
shows the loaded one.

**The 2.0 line.** Several limitations are fixed in the mssql extension's 2.0 line, which waits
for a DuckDB 2.0 release: `mssql_scan()` beside a catalog scan in a transaction (#314), stored
procedures through the TDS parser (#323), the per-table metadata query compiled once (#334). This
extension moves to that line with a bump of all three pins.

### Documentation versions

This site is versioned with the extension, from its tags: at every deploy the docs of each
release tag are snapshotted as that version — the dropdown serves the latest release, and the docs
being written for the next one live under *Next*. Nothing is committed for it; the tag is the
snapshot ([Releases](./releases.md)).
