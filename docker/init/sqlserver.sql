-- The DuckLake catalog database for the integration tests. DuckLake creates its own tables
-- (ducklake_metadata, ducklake_snapshot, ...) on the first ATTACH; the tests reset them between
-- runs. Idempotent: applied by the init container on every `make docker-up`, and by CI to the
-- bare service container.

USE master;
GO

IF DB_ID('lake_meta') IS NULL
BEGIN
    CREATE DATABASE lake_meta;
    PRINT 'lake_meta created';
END
ELSE
    PRINT 'lake_meta exists';
GO
