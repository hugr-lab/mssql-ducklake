---
title: Versions
sidebar_position: 6
---

# Versions and compatibility

One release line, bumped together in one commit:

| piece | pin | role |
| --- | --- | --- |
| DuckDB | **v2.0.0** | the release line the extension is built and distributed for |
| DuckLake | `main`, embedded at a named commit; catalog format **1.1** | compiled into the extension, unmodified |
| mssql extension | **v0.3.0** or newer at runtime | the runtime pair: `mssql_exec`/`mssql_scan`, the catalog attach, the TDS codecs |

**The embedded DuckLake bumps on this repository's schedule**, and a DuckLake fix reaches users
only with this extension's next release; every release names its DuckLake pin. Bumps are kept
cheap and frequent for that reason.

**mssql v0.3.0** is the first mssql build for DuckDB 2.0. `SELECT mssql_version();` shows the loaded
one. It brings several fixes that this extension relies on:

- `mssql_scan()` can run beside a catalog scan in a transaction (#314);
- stored procedures pass through the TDS parser (#323);
- the per-table metadata query is compiled once (#334);
- the server certificate is verified by default: `TrustServerCertificate=yes` for a self-signed one.

**The DuckDB v1.5.6 line** stays on v0.1.2 of this extension: DuckLake `v1.5-variegata`, catalog
format 1.0, mssql v0.2.5 or newer. The community repository serves the build that matches the
DuckDB you run. A catalog made by v0.1.x attaches to v0.2.0 as it is, at format 1.0. It moves to 1.1
only when asked to ([Releases](./releases.md)).

### Documentation versions

This site is versioned with the extension, from its tags: at every deploy the docs of each
release tag are snapshotted as that version — the dropdown serves the latest release, and the docs
being written for the next one live under *Next*. Nothing is committed for it; the tag is the
snapshot ([Releases](./releases.md)).
