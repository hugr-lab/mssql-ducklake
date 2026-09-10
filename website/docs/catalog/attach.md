---
title: Attaching
sidebar_position: 2
---

# Attaching a catalog

### Connection strings

```sql
-- ADO-style: everything after `ducklake:mssql:` goes to the mssql extension as it is
ATTACH 'ducklake:mssql:Server=host,1433;Database=lake_meta;User Id=app;Password=…;Encrypt=yes' AS lake
    (DATA_PATH 's3://bucket/lake/');

-- URI form: DuckDB does not treat `mssql://` as a prefix, so name the type
ATTACH 'ducklake:mssql://app:…@host:1433/lake_meta' AS lake
    (DATA_PATH 's3://bucket/lake/', META_TYPE 'mssql');
```

The connection string is the mssql extension's — its keys, encryption and authentication options
are documented [there](https://hugr-lab.github.io/mssql-extension/connection/). `Database` names the
catalog's database.

### Options

| option | meaning |
| --- | --- |
| `DATA_PATH` | where DuckLake writes data files. Required for a new catalog; an existing catalog pins the path it was created with. |
| `METADATA_SCHEMA` | the schema holding the catalog tables. Default `dbo` — a constant the mssql extension answers as the catalog's default schema, not the login's own. A login that must write elsewhere names its schema here. |
| `META_TYPE 'mssql'` | only for the `mssql://` URI form. |
| DuckLake's own | `DATA_INLINING_ROW_LIMIT`, `ENCRYPTED`, `SNAPSHOT_VERSION`, `SNAPSHOT_TIME`, … — this is DuckLake's `ATTACH`, so its [options](https://ducklake.select/docs/stable/duckdb/usage/connecting) apply unchanged. |

### Several lakes, one server

Two catalogs can share a database through `METADATA_SCHEMA`: each schema gets its own tables and
its own keys and indexes (their names embed the table, not the schema, and the shaping checks for
them per schema). A dedicated database per catalog is still the recommendation — the
[database option](./shaping.md#forced-parameterization) the shaping applies is database-wide.

One extension, several backends: the embedded DuckLake serves `ducklake:postgres:`, `ducklake:sqlite:`
and plain `ducklake:` file catalogs as well, in the same process as SQL Server ones.

### Re-attaching

Every attach after the first reads the catalog's shape stamp — an extended property on
`ducklake_metadata` — and does nothing else to the database unless the stamp is older than what this
build wants, in which case the [shaping](./shaping.md) runs again, idempotently. On a 1000-table
catalog an attach costs about 0.8 s, most of it the mssql extension discovering the schema.
