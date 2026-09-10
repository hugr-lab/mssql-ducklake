---
title: Getting Started
sidebar_position: 2
---

# Getting Started

### Prerequisites

- DuckDB **v1.5.5** — the release line this extension is built on ([Versions](./versions.md)).
- The **mssql extension v0.2.5 or newer**. Older versions answer DuckDB's `main` when DuckLake asks
  the catalog for its default schema, and the attach fails with `Schema 'main' not found`.
  `INSTALL mssql FROM community` gives a current version; `SELECT mssql_version()` says which one is
  loaded.
- **SQL Server 2019 or later, or Azure SQL Database** — the catalog's strings need a UTF-8 collation
  ([Requirements](./catalog/requirements.md)).
- A place for the data files: a local directory, S3, Azure Blob Storage — anything DuckDB can write
  to. DuckLake puts only the catalog in SQL Server.

### Step 1: install and load

```sql
INSTALL mssql FROM community;
INSTALL mssql_ducklake FROM community;
LOAD mssql_ducklake;          -- loads mssql as well, and refuses if stock ducklake is already loaded
```

Two rules that come from embedding DuckLake:

- **Never together with the stock `ducklake` extension.** Both provide the same functions and the
  same `ducklake:` attach prefix; `LOAD mssql_ducklake` refuses with a clear message when stock
  ducklake is loaded.
- **Load before the first `ATTACH 'ducklake:…'`.** DuckDB autoloads an extension by attach prefix,
  and for `ducklake:` that is the stock one; once `mssql_ducklake` is loaded the prefix is taken.

### Step 2: attach

```sql
ATTACH 'ducklake:mssql:Server=localhost,1433;Database=lake_meta;User Id=sa;Password=…' AS lake
    (DATA_PATH '/data/lake/');
```

`ducklake:mssql:` is followed by an ADO-style connection string, exactly what the mssql extension
takes. The URI form works too, with the type spelled out — DuckDB deliberately does not treat
`mssql://` as a prefix:

```sql
ATTACH 'ducklake:mssql://sa:…@localhost:1433/lake_meta' AS lake
    (DATA_PATH 's3://my-bucket/lake/', META_TYPE 'mssql');
```

The first attach creates the catalog: DuckLake's tables in the database's `dbo` schema, then the
keys, collations and indexes this extension adds ([Shaping](./catalog/shaping.md)). Every later
attach finds it and opens it. `METADATA_SCHEMA` puts the catalog in another schema; the other
[attach options](./catalog/attach.md) are DuckLake's own.

### Step 3: use it

```sql
CREATE SCHEMA lake.sales;
CREATE TABLE lake.sales.orders(id BIGINT, customer VARCHAR, amount DECIMAL(18, 2), day DATE);

INSERT INTO lake.sales.orders VALUES (1, 'acme', 10.5, DATE '2026-01-01');       -- inlined into the catalog
INSERT INTO lake.sales.orders
    SELECT i, 'c' || i, i * 1.5, DATE '2026-01-01' + i FROM range(10000) t(i);   -- a parquet file under DATA_PATH

UPDATE lake.sales.orders SET amount = amount * 2 WHERE id = 1;
DELETE FROM lake.sales.orders WHERE id > 9000;

SELECT * FROM ducklake_snapshots('lake');
SELECT count(*) FROM lake.sales.orders AT (VERSION => 2);
SELECT * FROM ducklake_table_info('lake');
```

Small inserts (10 rows by default) are **inlined** into the catalog — they land in a
`ducklake_inlined_data_*` table in SQL Server — and larger ones go to Parquet files under
`DATA_PATH`; `ducklake_flush_inlined_data('lake')` moves inlined rows to files. See
[Writing](./writing.md) for what is specific to SQL Server on the write side.

### Step 4: look at what landed

Attach the catalog database with the mssql extension directly and the catalog is ordinary tables:

```sql
ATTACH 'Server=localhost,1433;Database=lake_meta;User Id=sa;Password=…' AS meta (TYPE mssql);
SELECT table_name FROM meta.dbo.ducklake_table;
FROM mssql_scan('meta', 'SELECT name FROM sys.tables WHERE name LIKE ''ducklake%'' ORDER BY name');
```

Next: [the catalog in SQL Server](./catalog/index.md), [performance](./performance.md), and the
[settings](./reference/settings.md).
