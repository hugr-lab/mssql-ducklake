-- The DuckLake catalog database for the integration tests. The name arrives as the sqlcmd scripting
-- variable DB (`sqlcmd -v DB=lake_meta`, from MSSQL_DUCKLAKE_DB). DuckLake creates its own tables
-- (ducklake_metadata, ducklake_snapshot, ...) on the first ATTACH, and the tests drop every
-- `ducklake%` table between runs - so this script also plants a marker table, and a test refuses
-- to reset a database that does not carry it (a DSN pointed at a real catalog by mistake).
-- Idempotent: applied by `make docker-up` and by CI on every run.

USE master;
GO

IF DB_ID('$(DB)') IS NULL
BEGIN
    CREATE DATABASE [$(DB)];
    PRINT '$(DB) created';
END
ELSE
    PRINT '$(DB) exists';
GO

USE [$(DB)];
GO

IF OBJECT_ID('dbo.mssql_ducklake_test_env', 'U') IS NULL
BEGIN
    CREATE TABLE dbo.mssql_ducklake_test_env (note NVARCHAR(200) NOT NULL);
    INSERT INTO dbo.mssql_ducklake_test_env VALUES
        (N'created by docker/init/sqlserver.sql - the integration tests reset every ducklake% table here');
    PRINT 'test-environment marker planted';
END
GO
