#include "mssql_metadata_manager.hpp"
#include <map>
#include <set>
#include "mssql_metadata_internal.hpp"

#include "common/ducklake_types.hpp"
#include "common/ducklake_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/database.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_metadata_info.hpp"
#include "storage/ducklake_staged_commit.hpp"
#include "storage/ducklake_transaction_changes.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Initialization (specs/004 D3)
//===--------------------------------------------------------------------===//

MSSQLMetadataManager::CatalogMarkers MSSQLMetadataManager::ReadCatalogMarkers() {
	// One statement for the three questions an attach asks - is there a catalog yet, is its shape
	// this build's, has this build's migration run - so readiness costs one round trip.
	//
	// Both markers sit on ducklake_metadata - the catalog's own anchor table, the one DuckLake probes
	// to decide whether a catalog exists - and not on the schema. The shape stamp was on the schema
	// for one day, and that was a regression: a catalog whose tables were dropped and recreated kept
	// the schema's stamp, every later attach trusted it, and the catalog stayed without keys until
	// the first UPDATE failed. A marker on the table dies with the table, so a recreated catalog is
	// shaped (and migrated) again. Extended properties rather than rows in ducklake_metadata: DuckLake
	// reads every row of that table on every attach, and never reads these.
	//
	// The inner statement travels inside a duckdb string literal, so each of its own quotes is
	// doubled once; `mssql_scan_unsafe`, so bind sends nothing (mssql specs/081), and every column is
	// cast to what it declares so the two cannot drift. The marker as NVARCHAR: a VARCHAR result
	// takes the database's collation, and the extension warns about every non-UTF-8 one it reads.
	auto &connection = transaction.GetConnection();
	auto schema_name =
	    StringUtil::Replace(transaction.GetCatalog().MetadataSchemaName().GetIdentifierName(), "'", "''''");
	auto anchor = StringUtil::Format("OBJECT_ID(QUOTENAME(''%s'') + ''.ducklake_metadata'')", schema_name);
	auto property = [&](const char *name, const char *type) {
		return StringUtil::Format("(SELECT TRY_CAST(CAST(value AS NVARCHAR(400)) AS %s) FROM sys.extended_properties "
		                          "WHERE class = 1 AND major_id = %s AND minor_id = 0 AND name = ''%s'')",
		                          type, anchor, name);
	};
	auto result = TracedQuery(
	    connection,
	    StringUtil::Format(
	        "SELECT present, shape, migration, limits FROM mssql_scan_unsafe(%s, 'SELECT "
	        "CAST(CASE WHEN %s IS NULL THEN 0 ELSE 1 END AS BIGINT) AS present, %s AS shape, %s AS migration, "
	        "%s AS limits', columns := {'present': 'BIGINT', 'shape': 'BIGINT', 'migration': 'VARCHAR', 'limits': "
	        "'VARCHAR'})",
	        CatalogLiteral(), anchor, property(SHAPE_VERSION_PROPERTY, "BIGINT"),
	        property(MIGRATION_MARKER_PROPERTY, "NVARCHAR(64)"), property(LIMITS_PROPERTY, "NVARCHAR(400)")));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to inspect the DuckLake catalog on SQL Server: ");
	}
	CatalogMarkers markers;
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() == 0) {
		return markers;
	}
	markers.present = chunk->GetValue(0, 0).GetValue<int64_t>() != 0;
	auto shape = chunk->GetValue(1, 0);
	markers.shaped = !shape.IsNull();
	markers.shape_current = !shape.IsNull() && shape.GetValue<int64_t>() >= SHAPE_VERSION;
	auto limits = chunk->GetValue(3, 0);
	markers.limits = limits.IsNull() ? string() : limits.ToString();
	auto migration = chunk->GetValue(2, 0);
	markers.migration_current = !migration.IsNull() && migration.ToString() == MIGRATION_MARKER;
	return markers;
}

void MSSQLMetadataManager::RequireUtf8Collation() {
	// The catalog's string columns are stored with a UTF-8 collation, so the server has to have one.
	// Asking for the collation itself rather than the version: Azure SQL Database reports major
	// version 12 while supporting it, and the version was only ever a proxy for this question.
	// COUNT_BIG rather than COUNT: `COUNT(*)` is an `int` on the server and a given shape is checked
	// strictly, so declaring BIGINT over COUNT would fail at execution.
	auto &connection = transaction.GetConnection();
	auto probe = TracedQuery(
	    connection,
	    StringUtil::Format("SELECT collations FROM mssql_scan_unsafe(%s, 'SELECT COUNT_BIG(*) AS collations "
	                       "FROM sys.fn_helpcollations() WHERE name = ''%s''', columns := {'collations': 'BIGINT'})",
	                       CatalogLiteral(), VARCHAR_COLLATION));
	if (probe->HasError()) {
		probe->GetErrorObject().Throw("Failed to ask SQL Server for its collations: ");
	}
	auto row = probe->Fetch();
	if (!row || row->size() == 0 || row->GetValue(0, 0).IsNull() || row->GetValue(0, 0).GetValue<int64_t>() == 0) {
		throw NotImplementedException(
		    "This SQL Server has no %s collation, which a DuckLake catalog needs to store its strings without loss. "
		    "UTF-8 collations arrived in SQL Server 2019.",
		    VARCHAR_COLLATION);
	}
}

//===--------------------------------------------------------------------===//
// The 1.0 -> 1.1-dev1 migration, in T-SQL (design/005)
//===--------------------------------------------------------------------===//

//! Why this is ours. DuckLake's migration is `ALTER TABLE … ADD COLUMN {IF_NOT_EXISTS} …` plus a
//! `CREATE TABLE {IF_NOT_EXISTS}`, and it goes to duckdb, which sends it on to the catalog: the
//! mssql extension's ALTER has no form for `IF NOT EXISTS`, so the guard is dropped and attaching a
//! 1.0 catalog that is already part-way through the migration dies on the first statement -
//!
//!   SQL Server error 2705: Column names in each table must be unique.
//!   Column name 'row_group_count' in table 'dbo.ducklake_data_file' is specified more than once.
//!
//! - which `MigrateV10Dev` turns into a warning, leaving the catalog at 1.0 for good. So the three
//! migration virtuals are overridden and none of their SQL is touched: the same migration, written
//! the way the shaping writes everything, each step guarded by the server's own catalog views, so a
//! re-run is a no-op instead of an error. The pre-1.0 migrations have the same problem and are
//! left alone: every catalog a release of this extension created is at 1.0 or later, because 1.0 was
//! the latest format on the line it shipped on.
void MSSQLMetadataManager::MigrateToV1_1Dev1() {
	// META_LIMITS narrows a catalog only on its way from 1.0 (specs/018): DuckLake re-runs this for a
	// dev format on every writable attach, and a catalog already at 1.1 lives with the limits it has.
	// Read before anything below changes the version.
	const bool from_v1_0 = CatalogVersion() == "1.0";
	// and checked before it too: a META_LIMITS shorter than what is stored fails the attach while the
	// catalog is still 1.0, so the next attach meets the same migration rather than a half-done one
	if (from_v1_0) {
		CheckMigrationLimits();
	}
	// renaming first means a collision in a user's inlined table aborts while the catalog still
	// says 1.0, which is the order upstream picked for the same reason
	MigrateInlinedColumnNames(true);

	const string schema = SchemaIdentifier();
	const string schema_literal = SchemaLiteral();

	// the columns 1.1-dev1 adds, in this catalog's types rather than DuckLake's spelling: BOOLEAN is
	// BIT here
	const vector<pair<string, string>> added_columns = {
	    {"ducklake_data_file", "row_group_count BIGINT"},    {"ducklake_delete_file", "row_group_count BIGINT"},
	    {"ducklake_file_column_stats", "min_is_exact BIT"},  {"ducklake_file_column_stats", "max_is_exact BIT"},
	    {"ducklake_table_column_stats", "min_is_exact BIT"}, {"ducklake_table_column_stats", "max_is_exact BIT"},
	    {"ducklake_schema", "parent_schema_id BIGINT"},
	};
	// @changed records whether this run added anything. A catalog created at 1.1 already has it all,
	// and its first attach would otherwise pay a whole re-shaping for nothing (specs/015 R3).
	string ddl = "DECLARE @changed BIT = 0;\n";
	for (auto &entry : added_columns) {
		auto column = StringUtil::Split(entry.second, ' ')[0];
		ddl += StringUtil::Format("IF COL_LENGTH(QUOTENAME(%s) + '.%s', '%s') IS NULL "
		                          "BEGIN ALTER TABLE %s.%s ADD %s; SET @changed = 1; END;\n",
		                          schema_literal, entry.first, column, schema, entry.first, entry.second);
	}
	// the table 1.1 adds - with our VARCHAR collation, like every other string column in the catalog,
	// and `key` quoted because it is a T-SQL keyword
	ddl += StringUtil::Format("IF OBJECT_ID(QUOTENAME(%s) + '.ducklake_view_column_tag') IS NULL "
	                          "BEGIN CREATE TABLE %s.ducklake_view_column_tag(view_id BIGINT, "
	                          "column_name VARCHAR(MAX) COLLATE %s, begin_snapshot BIGINT, end_snapshot BIGINT, "
	                          "[key] VARCHAR(MAX) COLLATE %s, value VARCHAR(MAX) COLLATE %s); SET @changed = 1; END;\n",
	                          schema_literal, schema, VARCHAR_COLLATION, VARCHAR_COLLATION, VARCHAR_COLLATION);
	// the value is named as well as the key: this runs on a 1.0 or an already-migrated catalog and
	// nothing else, so a stray call cannot relabel a catalog of some later format as this one
	ddl += StringUtil::Format("UPDATE %s.ducklake_metadata SET value = N'1.1-dev1' "
	                          "WHERE [key] = N'version' AND value IN (N'1.0', N'1.1-dev1');\n",
	                          schema);
	// the marker, last: this build's migration has run on this catalog (specs/015 R3)
	ddl += StringUtil::Format(
	    "IF EXISTS (SELECT 1 FROM sys.extended_properties WHERE class = 1 AND major_id = "
	    "OBJECT_ID(QUOTENAME(%s) + '.ducklake_metadata') AND minor_id = 0 AND name = '%s') "
	    "EXEC sp_updateextendedproperty @name = N'%s', @value = N'%s', @level0type = N'SCHEMA', @level0name = %s, "
	    "@level1type = N'TABLE', @level1name = N'ducklake_metadata' "
	    "ELSE EXEC sp_addextendedproperty @name = N'%s', @value = N'%s', @level0type = N'SCHEMA', @level0name = %s, "
	    "@level1type = N'TABLE', @level1name = N'ducklake_metadata';\n",
	    schema_literal, MIGRATION_MARKER_PROPERTY, MIGRATION_MARKER_PROPERTY, MIGRATION_MARKER, schema_literal,
	    MIGRATION_MARKER_PROPERTY, MIGRATION_MARKER, schema_literal);
	// what this run added has not been keyed or collated yet: the stamp goes, only then
	ddl += StringUtil::Format(
	    "IF @changed = 1 AND EXISTS (SELECT 1 FROM sys.extended_properties WHERE class = 1 AND major_id = "
	    "OBJECT_ID(QUOTENAME(%s) + '.ducklake_metadata') AND minor_id = 0 AND name = '%s') "
	    "EXEC sp_dropextendedproperty @name = N'%s', @level0type = N'SCHEMA', @level0name = %s, "
	    "@level1type = N'TABLE', @level1name = N'ducklake_metadata';\n",
	    schema_literal, SHAPE_VERSION_PROPERTY, SHAPE_VERSION_PROPERTY, schema_literal);

	RunServerSideOutsideTransaction(ddl, "Failed to migrate the DuckLake catalog to v1.1-dev1: ");
	// the limits the ATTACH that migrates asks for (specs/018) - before the shaping, which reads them
	if (from_v1_0) {
		ApplyMigrationLimits();
	}
	// The metadata database is preloaded at its ATTACH (specs/015), so a table just altered is cached
	// as it was - ducklake_schema without parent_schema_id. Those tables, and only those, are dropped
	// from the cache here, while this transaction holds no lock on them (the migration ran on a
	// connection of its own); the rest of the preload stays. Nothing added, nothing dropped.
	const bool changed = !ReadCatalogMarkers().shape_current;
	std::set<string> altered;
	if (changed) {
		for (auto &entry : added_columns) {
			altered.insert(entry.first);
		}
		altered.insert("ducklake_view_column_tag");
	}
	if (from_v1_0) {
		auto requested = RequestedLengths(transaction.GetCatalog().GetAttached());
		auto given = [&](LengthClass length_class) {
			switch (length_class) {
			case LengthClass::NAME:
				return requested.name != CatalogLengths::NOT_GIVEN;
			case LengthClass::PATH:
				return requested.path != CatalogLengths::NOT_GIVEN;
			case LengthClass::COLUMN_TYPE:
				return requested.column_type != CatalogLengths::NOT_GIVEN;
			case LengthClass::DEFAULT_VALUE:
				return requested.default_value != CatalogLengths::NOT_GIVEN;
			case LengthClass::TEXT:
				return requested.text != CatalogLengths::NOT_GIVEN;
			case LengthClass::STATS:
				return requested.stats != CatalogLengths::NOT_GIVEN;
			default:
				return false;
			}
		};
		for (auto &column : CatalogStringColumns()) {
			if (given(column.length_class)) {
				altered.insert(column.table);
			}
		}
	}
	for (auto &table : altered) {
		InvalidateTableCache(table);
	}
	// The migration runs on this manager in the attach transaction, before DuckLake swaps it for a
	// stock one (specs/015 R1), so it shapes its own result here rather than leaving it to a later
	// attach - and only when the run added something, which the stamp now says.
	if (changed) {
		EnsureCatalogShape();
	}
}

//! Format 1.1 prefixes the metadata columns of the inlined DATA tables with `_ducklake_` - those
//! only: the inlined deletion tables keep the bare names (specs/006 D5b, and ducklake writes that
//! DDL bare too). One dynamic batch renames every table the catalog's own registry lists, so it is
//! a no-op the second time without a probe; `sp_rename` carries the primary key along with the
//! column. `probe_renamed` has nothing to switch off here - the batch costs one round trip either
//! way.
void MSSQLMetadataManager::MigrateInlinedColumnNames(bool probe_renamed) {
	const string schema = SchemaIdentifier();
	const string schema_literal = SchemaLiteral();
	// the metadata columns are the table's first three by construction, which is also how DuckLake
	// finds them - so `column_id <= 3` keeps a user column that happens to be called `row_id` out of
	// it, and a user column already called `_ducklake_row_id` makes sp_rename fail, as it should.
	// The join needs a collation: `table_name` is the catalog's own VARCHAR under the UTF-8 BIN2
	// collation and `sys.tables.name` is sysname under the database's, which `=` refuses to mix.
	// STRING_AGG over a DISTINCT derived table rather than the `SELECT @sql += ...` the shaping uses:
	// a registry row per schema version means the same table is listed more than once, and assigning
	// to a variable in a SELECT that also says DISTINCT has no defined result - it renamed nothing.
	// All of the renames or none: a user column colliding with a prefixed name fails one sp_rename,
	// and a catalog left half-prefixed is one a 1.0 build can no longer read
	// the tables it will rename, read first: their cached entries (preloaded at the ATTACH) are the
	// ones to drop afterwards, and none at all when there is nothing to rename
	auto listing = TracedQuery(
	    transaction.GetConnection(),
	    StringUtil::Format(
	        "SELECT name FROM mssql_scan_unsafe(%s, %s, columns := {'name': 'VARCHAR'})", CatalogLiteral(),
	        SQLString::ToString(StringUtil::Format(
	            "SELECT DISTINCT CAST(t.name AS NVARCHAR(128)) AS name FROM %s.ducklake_inlined_data_tables idt "
	            "JOIN sys.tables t ON t.name = idt.table_name COLLATE DATABASE_DEFAULT AND t.schema_id = "
	            "SCHEMA_ID(%s) JOIN sys.columns c ON c.object_id = t.object_id AND c.column_id <= 3 "
	            "WHERE c.name IN ('row_id', 'begin_snapshot', 'end_snapshot')",
	            schema, schema_literal))));
	if (listing->HasError()) {
		listing->GetErrorObject().Throw("Failed to list the inlined tables to rename to v1.1-dev1: ");
	}
	vector<string> renamed;
	while (auto chunk = listing->Fetch()) {
		for (idx_t row = 0; row < chunk->size(); row++) {
			renamed.push_back(chunk->GetValue(0, row).ToString());
		}
	}
	if (renamed.empty()) {
		return;
	}
	auto statement = StringUtil::Format(R"(
DECLARE @rename NVARCHAR(MAX);
SELECT @rename = STRING_AGG(CAST(stmt AS NVARCHAR(MAX)), CHAR(10))
FROM (SELECT DISTINCT N'EXEC sp_rename N' + CHAR(39) + QUOTENAME(%s) + N'.' + QUOTENAME(t.name) + N'.'
                    + QUOTENAME(c.name) + CHAR(39) + N', N' + CHAR(39) + N'_ducklake_' + c.name + CHAR(39)
                    + N', N' + CHAR(39) + N'COLUMN' + CHAR(39) + N';' AS stmt
      FROM %s.ducklake_inlined_data_tables idt
      JOIN sys.tables t ON t.name = idt.table_name COLLATE DATABASE_DEFAULT AND t.schema_id = SCHEMA_ID(%s)
      JOIN sys.columns c ON c.object_id = t.object_id AND c.column_id <= 3
      WHERE c.name IN ('row_id', 'begin_snapshot', 'end_snapshot')) renames;
IF @rename IS NOT NULL
BEGIN
	SET XACT_ABORT ON;
	BEGIN TRANSACTION;
	EXEC sp_executesql @rename;
	COMMIT TRANSACTION;
END;
)",
	                                    schema_literal, schema, schema_literal);
	RunServerSideOutsideTransaction(statement, "Failed to rename the inlined metadata columns to v1.1-dev1: ");
	// the renamed tables' cached columns are the old names (see MigrateToV1_1Dev1)
	for (auto &table : renamed) {
		InvalidateTableCache(table);
	}
}

//! The explicit path, `ATTACH … (AUTOMATIC_MIGRATION TRUE)`: every statement is guarded, so a
//! failure here is a real one and is left to surface.
void MSSQLMetadataManager::MigrateV10(bool allow_failures) {
	MigrateToV1_1Dev1();
}

//! The attach-time path, and DuckLake runs it on EVERY writable attach: `1.1-dev1` is a development
//! format, its version string does not move when upstream adds another column under the same name,
//! so a catalog stamped by an earlier build can be missing a later addition and re-applying is the
//! only way to know (ducklake_initializer.cpp). Upstream's re-run is nine cheap duckdb statements.
//! Ours is one server-side batch - but it ends by dropping the shape stamp, and that made
//! EnsureCatalogShape re-shape the whole catalog on every attach: 0.8 s of the 1.8 s an attach of a
//! 1000-table catalog cost, measured by scripts/bench/attach_probe.py. So the stamp answers the
//! question instead of being invalidated by it: it is written after the shaping, which runs after
//! the migration, so a current stamp means a build wanting this shape has already done both. The
//! version is 1.1-dev1 here by construction - that is the branch DuckLake took to get here - and a
//! 1.0 catalog arrives at MigrateV10, which always runs.
//!
//! The marker answers that, not the shape stamp (specs/015 R3): they move independently.
//!
//! Upstream also splits the re-run in two halves and logs a warning for each, because either can
//! fail on a catalog it has already half-migrated; ours cannot, so there is nothing to swallow and
//! the error is the answer here too.
void MSSQLMetadataManager::MigrateV10Dev() {
	if (ReadCatalogMarkers().migration_current) {
		return;
	}
	MigrateToV1_1Dev1();
}

string MSSQLMetadataManager::CatalogVersion() {
	auto result = TracedQuery(
	    transaction.GetConnection(),
	    StringUtil::Format(
	        "SELECT v FROM mssql_scan_unsafe(%s, 'SELECT CAST([value] AS NVARCHAR(64)) AS v FROM %s.ducklake_metadata "
	        "WHERE [key] = N''version''', columns := {'v': 'VARCHAR'})",
	        CatalogLiteral(), StringUtil::Replace(SchemaIdentifier(), "'", "''")));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to read the DuckLake catalog's version: ");
	}
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() == 0) {
		return string();
	}
	return chunk->GetValue(0, 0).ToString();
}

string MSSQLMetadataManager::MigrationLimitTargets() {
	auto requested = RequestedLengths(transaction.GetCatalog().GetAttached());
	if (!requested.AnyGiven()) {
		return string();
	}

	// The columns the ATTACH names a length for, as (table, column, bytes; 0 for MAX).
	string targets;
	for (auto &column : CatalogStringColumns()) {
		int64_t given = CatalogLengths::NOT_GIVEN;
		switch (column.length_class) {
		case LengthClass::NAME:
			given = requested.name;
			break;
		case LengthClass::PATH:
			given = requested.path;
			break;
		case LengthClass::COLUMN_TYPE:
			given = requested.column_type;
			break;
		case LengthClass::DEFAULT_VALUE:
			given = requested.default_value;
			break;
		case LengthClass::TEXT:
			given = requested.text;
			break;
		case LengthClass::STATS:
			given = requested.stats;
			break;
		default:
			break;
		}
		if (given == CatalogLengths::NOT_GIVEN) {
			continue;
		}
		targets += StringUtil::Format("%s(N'%s', N'%s', %lld)", targets.empty() ? "" : ", ", column.table,
		                              column.column, given);
	}
	return targets;
}

void MSSQLMetadataManager::CheckMigrationLimits() {
	auto targets = MigrationLimitTargets();
	if (targets.empty()) {
		return;
	}
	const string schema_literal = SchemaLiteral();
	// What is stored first: a value longer than its new bound fails the attach here, naming it,
	// before anything is altered. Lengths in bytes of UTF-8 - what the column will hold - whatever
	// the column is now (VARCHAR(MAX) from an older shaping, NVARCHAR from none).
	auto check = StringUtil::Format(R"(
DECLARE @sql NVARCHAR(MAX) = N'';
SELECT @sql += CASE WHEN @sql = N'' THEN N'' ELSE N' UNION ALL ' END
  + N'SELECT CAST(N''' + t.name + N''' AS NVARCHAR(128)) AS tbl, CAST(N''' + c.name + N''' AS NVARCHAR(128)) AS col, '
  + CAST(m.bytes AS NVARCHAR(10)) + N' AS bound, CAST(ISNULL(MAX(DATALENGTH(CAST(CAST(' + QUOTENAME(c.name)
  + N' AS NVARCHAR(MAX)) COLLATE %s AS VARCHAR(MAX)))), 0) AS BIGINT) AS longest FROM ' + QUOTENAME(s.name) + N'.' + QUOTENAME(t.name)
FROM sys.columns c JOIN sys.tables t ON t.object_id = c.object_id JOIN sys.schemas s ON s.schema_id = t.schema_id
JOIN (VALUES %s) m(tbl, col, bytes) ON m.tbl = t.name AND m.col = c.name
WHERE s.name = %s AND m.bytes > 0;
IF @sql = N'' SET @sql = N'SELECT CAST(NULL AS NVARCHAR(128)) AS tbl, CAST(NULL AS NVARCHAR(128)) AS col, 0 AS bound, CAST(0 AS BIGINT) AS longest WHERE 1 = 0';
SET @sql = N'SELECT tbl, col, bound, longest FROM (' + @sql + N') x WHERE longest > bound';
EXEC sp_executesql @sql;)",
	                                VARCHAR_COLLATION, targets, schema_literal);
	auto result = TracedQuery(transaction.GetConnection(),
	                          StringUtil::Format("SELECT * FROM mssql_scan_unsafe(%s, %s, columns := {'tbl': "
	                                             "'VARCHAR', 'col': 'VARCHAR', 'bound': 'INTEGER', "
	                                             "'longest': 'BIGINT'})",
	                                             CatalogLiteral(), SQLString::ToString(check)));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to check the DuckLake catalog's strings against META_LIMITS: ");
	}
	string too_long;
	while (auto chunk = result->Fetch()) {
		for (idx_t row = 0; row < chunk->size(); row++) {
			too_long += StringUtil::Format("\n  %s.%s: a value of %s bytes, the bound %s",
			                               chunk->GetValue(0, row).ToString(), chunk->GetValue(1, row).ToString(),
			                               chunk->GetValue(3, row).ToString(), chunk->GetValue(2, row).ToString());
		}
	}
	if (!too_long.empty()) {
		throw InvalidInputException("mssql_ducklake: META_LIMITS is shorter than what the catalog already holds - "
		                            "nothing was changed:%s\nRaise those limits, or migrate without them.",
		                            too_long);
	}
}

void MSSQLMetadataManager::ApplyMigrationLimits() {
	auto targets = MigrationLimitTargets();
	if (targets.empty()) {
		return;
	}
	const string schema_literal = SchemaLiteral();
	auto markers = ReadCatalogMarkers();
	auto recorded = markers.limits.empty() ? CatalogLengths() : CatalogLengths::Parse(markers.limits);
	auto target = recorded.WithMaxForMissing().OverriddenBy(RequestedLengths(transaction.GetCatalog().GetAttached()));
	// Then the columns, and the limits the catalog now lives with.
	auto alter =
	    StringUtil::Format(R"(
DECLARE @alter NVARCHAR(MAX) = N'';
SELECT @alter += N'ALTER TABLE ' + QUOTENAME(s.name) + N'.' + QUOTENAME(t.name) + N' ALTER COLUMN ' + QUOTENAME(c.name)
  + CASE WHEN m.bytes = 0 THEN N' VARCHAR(MAX)' ELSE N' VARCHAR(' + CAST(m.bytes AS NVARCHAR(10)) + N')' END
  + N' COLLATE %s' + CASE WHEN c.is_nullable = 0 THEN N' NOT NULL' ELSE N' NULL' END + N';'
FROM sys.columns c JOIN sys.tables t ON t.object_id = c.object_id JOIN sys.schemas s ON s.schema_id = t.schema_id
JOIN (VALUES %s) m(tbl, col, bytes) ON m.tbl = t.name AND m.col = c.name
WHERE s.name = %s;
EXEC sp_executesql @alter;
IF EXISTS (SELECT 1 FROM sys.extended_properties WHERE class = 1 AND major_id = OBJECT_ID(QUOTENAME(%s) + '.ducklake_metadata') AND minor_id = 0 AND name = '%s')
    EXEC sp_updateextendedproperty @name = N'%s', @value = N'%s', @level0type = N'SCHEMA', @level0name = %s, @level1type = N'TABLE', @level1name = N'ducklake_metadata';
ELSE
    EXEC sp_addextendedproperty @name = N'%s', @value = N'%s', @level0type = N'SCHEMA', @level0name = %s, @level1type = N'TABLE', @level1name = N'ducklake_metadata';
)",
	                       VARCHAR_COLLATION, targets, schema_literal, schema_literal, LIMITS_PROPERTY, LIMITS_PROPERTY,
	                       target.Serialize(), schema_literal, LIMITS_PROPERTY, target.Serialize(), schema_literal);
	RunServerSideOutsideTransaction(alter, "Failed to apply META_LIMITS to the DuckLake catalog: ");
}

CatalogLengths MSSQLMetadataManager::ResolveCatalogLengths() {
	auto markers = ReadCatalogMarkers();
	auto requested = RequestedLengths(transaction.GetCatalog().GetAttached());
	if (!markers.limits.empty()) {
		// chosen once, and the catalog lives with them: an ATTACH asking for others changes nothing
		auto recorded = CatalogLengths::Parse(markers.limits).WithMaxForMissing();
		if (requested.AnyGiven() && recorded.OverriddenBy(requested) != recorded) {
			auto client_context = transaction.context.lock();
			if (client_context) {
				DUCKDB_LOG_WARNING(*client_context,
				                   StringUtil::Format("mssql_ducklake: META_LIMITS ignored - the catalog was created "
				                                      "with %s, and its limits change only when it is migrated "
				                                      "(specs/018)",
				                                      recorded.Serialize()));
			}
		}
		return recorded;
	}
	if (!markers.shaped) {
		// never shaped: a new catalog, or one a stock manager created and used - only the first may
		// be narrowed without looking at what it holds
		auto result = TracedQuery(
		    transaction.GetConnection(),
		    StringUtil::Format(
		        "SELECT s FROM mssql_scan_unsafe(%s, 'SELECT CAST(ISNULL(MAX(snapshot_id), 0) AS BIGINT) AS s FROM "
		        "%s.ducklake_snapshot', columns := {'s': 'BIGINT'})",
		        CatalogLiteral(), StringUtil::Replace(SchemaIdentifier(), "'", "''")));
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to inspect the DuckLake catalog on SQL Server: ");
		}
		auto chunk = result->Fetch();
		if (chunk && chunk->size() == 1 && chunk->GetValue(0, 0).GetValue<int64_t>() == 0) {
			return requested.WithDefaults();
		}
	}
	// a catalog older than the limits keeps MAX - the migration from 1.0 narrows it when asked
	return CatalogLengths().WithMaxForMissing();
}

namespace {
//! A T-SQL bracketed identifier, as QUOTENAME writes one
string QuoteName(const string &name) {
	return "[" + StringUtil::Replace(name, "]", "]]") + "]";
}
} // namespace

MSSQLMetadataManager::ShapeState MSSQLMetadataManager::ReadShapeState() {
	auto schema_name =
	    StringUtil::Replace(transaction.GetCatalog().MetadataSchemaName().GetIdentifierName(), "'", "''");
	auto in_schema = StringUtil::Format("t.schema_id = SCHEMA_ID(N'%s') AND t.name LIKE N'ducklake%%'", schema_name);
	auto tsql = StringUtil::Format(
	    "SELECT CAST(N'T' AS NVARCHAR(2)) AS kind, CAST(t.name AS NVARCHAR(128)) AS tbl, CAST(N'' AS NVARCHAR(128)) "
	    "AS name, CAST(N'' AS NVARCHAR(400)) AS info, CAST(0 AS BIGINT) AS flag FROM sys.tables t WHERE %s "
	    "UNION ALL SELECT N'K', t.name, kc.name, ISNULL((SELECT STRING_AGG(CAST(c.name AS NVARCHAR(MAX)), N',') "
	    "WITHIN GROUP (ORDER BY ic.key_ordinal) FROM sys.index_columns ic JOIN sys.columns c ON c.object_id = "
	    "ic.object_id AND c.column_id = ic.column_id WHERE ic.object_id = kc.parent_object_id AND ic.index_id = "
	    "kc.unique_index_id), N''), 0 FROM sys.key_constraints kc JOIN sys.tables t ON t.object_id = "
	    "kc.parent_object_id WHERE kc.type = 'PK' AND %s "
	    "UNION ALL SELECT N'I', t.name, i.name, N'', 0 FROM sys.indexes i JOIN sys.tables t ON t.object_id = "
	    "i.object_id WHERE i.name LIKE N'ix[_]%%' AND %s "
	    "UNION ALL SELECT N'C', t.name, c.name, ty.name + N'|' + CAST(c.max_length AS NVARCHAR(10)) + N'|' + "
	    "ISNULL(c.collation_name, N''), CAST(c.is_nullable AS BIGINT) FROM sys.columns c JOIN sys.tables t ON "
	    "t.object_id = c.object_id JOIN sys.types ty ON ty.user_type_id = c.user_type_id WHERE ty.name IN "
	    "(N'nvarchar', N'nchar', N'ntext', N'varchar', N'char') AND (ty.name LIKE N'n%%' OR t.name NOT LIKE "
	    "N'ducklake[_]inlined[_]%%') AND %s "
	    "UNION ALL SELECT N'P', N'', p.name, N'', CAST(p.class AS BIGINT) FROM sys.extended_properties p WHERE "
	    "(p.class = 1 AND p.major_id = OBJECT_ID(QUOTENAME(N'%s') + N'.ducklake_metadata') AND p.minor_id = 0) OR "
	    "(p.class = 3 AND p.major_id = SCHEMA_ID(N'%s') AND p.name = N'%s')",
	    in_schema, in_schema, in_schema, in_schema, schema_name, schema_name, SHAPE_VERSION_PROPERTY);
	auto result = TracedQuery(
	    transaction.GetConnection(),
	    StringUtil::Format("SELECT kind, tbl, name, info, flag FROM mssql_scan_unsafe(%s, %s, columns := {'kind': "
	                       "'VARCHAR', 'tbl': 'VARCHAR', 'name': 'VARCHAR', 'info': 'VARCHAR', 'flag': 'BIGINT'})",
	                       CatalogLiteral(), SQLString::ToString(tsql)));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to inspect the DuckLake catalog's shape on SQL Server: ");
	}
	ShapeState state;
	while (auto chunk = result->Fetch()) {
		for (idx_t row = 0; row < chunk->size(); row++) {
			auto kind = chunk->GetValue(0, row).ToString();
			auto table = chunk->GetValue(1, row).ToString();
			auto name = chunk->GetValue(2, row).ToString();
			auto info = chunk->GetValue(3, row).ToString();
			auto flag = chunk->GetValue(4, row).GetValue<int64_t>();
			if (kind == "T") {
				state.tables.insert(table);
			} else if (kind == "K") {
				state.keys[table] = {name, info};
			} else if (kind == "I") {
				state.indexes.insert(name);
			} else if (kind == "C") {
				auto parts = StringUtil::Split(info, '|');
				ShapeColumn column;
				column.type = parts.size() > 0 ? parts[0] : string();
				column.max_length = parts.size() > 1 ? std::stoll(parts[1]) : 0;
				column.collation = parts.size() > 2 ? parts[2] : string();
				column.nullable = flag != 0;
				state.columns[{table, name}] = column;
			} else if (kind == "P") {
				if (flag == 3) {
					state.schema_stamp = true;
				} else {
					state.properties.insert(name);
				}
			}
		}
	}
	return state;
}

void MSSQLMetadataManager::EnsureCatalogShape() {
	RequireUtf8Collation();
	const string schema = SchemaIdentifier();
	const string schema_literal =
	    SQLString::ToString(transaction.GetCatalog().MetadataSchemaName().GetIdentifierName());
	const auto lengths = ResolveCatalogLengths();

	// Primary keys. DuckLake declares a few itself; the rest are ours, and they are what make the
	// catalog writable at all: the mssql extension builds a row identity out of the primary key, and
	// without one it refuses every UPDATE and DELETE - which commits, expiry, cleanup and compaction
	// are full of. The list is every table DuckLake updates or deletes from.
	const vector<pair<string, string>> keys = {
	    {"ducklake_metadata", "[key]"},
	    {"ducklake_table_stats", "table_id"},
	    {"ducklake_table_column_stats", "table_id, column_id"},
	    {"ducklake_table", "table_id, begin_snapshot"},
	    {"ducklake_view", "view_id, begin_snapshot"},
	    {"ducklake_column", "table_id, column_id, begin_snapshot"},
	    {"ducklake_tag", "object_id, begin_snapshot, [key]"},
	    {"ducklake_column_tag", "table_id, column_id, begin_snapshot, [key]"},
	    // 1.1: the same shape for a view's column tags, keyed on the column name
	    {"ducklake_view_column_tag", "view_id, column_name, begin_snapshot, [key]"},
	    {"ducklake_partition_info", "partition_id"},
	    {"ducklake_partition_column", "partition_id, partition_key_index"},
	    {"ducklake_sort_info", "sort_id"},
	    {"ducklake_sort_expression", "sort_id, sort_key_index"},
	    {"ducklake_macro", "macro_id, begin_snapshot"},
	    {"ducklake_macro_impl", "macro_id, impl_id"},
	    {"ducklake_macro_parameters", "macro_id, impl_id, column_id"},
	    {"ducklake_inlined_data_tables", "table_id, schema_version"},
	    {"ducklake_files_scheduled_for_deletion", "data_file_id"},
	    {"ducklake_file_column_stats", "data_file_id, column_id"},
	    {"ducklake_file_variant_stats", "data_file_id, column_id, variant_path"},
	    {"ducklake_file_partition_value", "data_file_id, partition_key_index"},
	    {"ducklake_column_mapping", "mapping_id"},
	    {"ducklake_name_mapping", "mapping_id, column_id"},
	    // one row per table whose schema changed in the snapshot, so the table is what tells two rows
	    // of one snapshot apart (DuckLakeMetadataManager::InsertNewSchema; issue #30)
	    {"ducklake_schema_versions", "begin_snapshot, schema_version, table_id"},
	};
	// The tag tables and the metadata table key on a name rather than an id. A primary key cannot be
	// over MAX, so those are capped - 200 bytes of UTF-8 is a long name and well inside the
	// 900-byte index limit.
	auto key_column_type = [&](const string &name) {
		if (name == "[key]" || name == "variant_path" || name == "column_name") {
			return StringUtil::Format("VARCHAR(200) COLLATE %s", VARCHAR_COLLATION);
		}
		return string("BIGINT");
	};

	// What the server holds, read once (specs/015): the tables, their primary keys and key columns,
	// the indexes, the string columns and the catalog's properties. The DDL below is generated from
	// it and holds no guard of its own. It used to: each `IF EXISTS (SELECT ... FROM sys.key_constraints
	// ...)` was a statement over the catalog views, and SQL Server compiles a batch whole - ~25 of
	// them at ~33 ms each made shaping a new catalog 1.2 s, against 0.3 s for postgres's whole first
	// phase. A table one format added is not in a catalog of an older one, and the attach that
	// migrates shapes on the same pass: the server says what is there, the version only what should.
	//
	// Two nodes attaching one older-shaped catalog at once would both read "missing" and both add it;
	// the guards that used to make that harmless are gone with the compiles they cost. An exclusive
	// application lock on the schema, held to the end of this transaction, makes the second wait and
	// then read the state the first committed - and find nothing to do.
	RunServerSide(StringUtil::Format("DECLARE @r INT; EXEC @r = sp_getapplock @Resource = N'mssql_ducklake_shape:%s', "
	                                 "@LockMode = 'Exclusive', @LockOwner = 'Transaction', @LockTimeout = 120000; "
	                                 "IF @r < 0 THROW 50000, 'mssql_ducklake: timed out waiting for another node "
	                                 "shaping this catalog', 1;",
	                                 StringUtil::Replace(
	                                     transaction.GetCatalog().MetadataSchemaName().GetIdentifierName(), "'", "''")),
	              "Failed to take the shaping lock on the DuckLake catalog: ");
	auto state = ReadShapeState();
	auto has_table = [&](const string &table) {
		return state.tables.count(table) > 0;
	};
	auto has_index = [&](const string &index) {
		return state.indexes.count(index) > 0;
	};
	// a column given an explicit ALTER below, which the string sweep must then leave alone - the sweep
	// used to read sys.columns after those ALTERs had run, and now reads the state from before them
	std::set<pair<string, string>> altered;
	// the tables this shaping changes: the metadata database is preloaded at its ATTACH (specs/015),
	// so their cached entries - keys, column types - are stale once it has run, and only theirs
	std::set<string> touched;

	string columns_ddl;
	string constraints_ddl;

	for (auto &entry : keys) {
		if (!has_table(entry.first)) {
			continue;
		}
		auto names = StringUtil::Split(entry.second, ',');
		// the key as the server spells one: the column names in key order, without the brackets a
		// reserved word wears here
		string declared;
		for (auto &name : names) {
			StringUtil::Trim(name);
			if (!declared.empty()) {
				declared += ",";
			}
			declared += StringUtil::Replace(StringUtil::Replace(name, "[", ""), "]", "");
		}
		auto key = state.keys.find(entry.first);
		const auto key_name = "pk_" + entry.first;
		if (key != state.keys.end() && key->second.first == key_name && key->second.second == declared) {
			continue; // keyed as this build keys it
		}
		if (key != state.keys.end() && key->second.first == key_name) {
			// A key this build has since corrected, still on the table under the same name (issue
			// #30: ducklake_schema_versions keyed without table_id refused every commit touching two
			// tables). Dropped and made again.
			columns_ddl += StringUtil::Format("ALTER TABLE %s.%s DROP CONSTRAINT %s;\n", schema, entry.first, key_name);
		}
		if (entry.first == "ducklake_schema_versions") {
			// DuckLake's own migration adds table_id nullable, fills it from history and deletes the
			// rows it could not fill; a catalog whose migration did not finish still holds them, and
			// NULLs keep the column out of a key
			columns_ddl +=
			    StringUtil::Format("DELETE FROM %s.ducklake_schema_versions WHERE table_id IS NULL;\n", schema);
		}
		for (auto &name : names) {
			// a key column has to be NOT NULL, and DuckLake declares none of them so
			columns_ddl += StringUtil::Format("ALTER TABLE %s.%s ALTER COLUMN %s %s NOT NULL;\n", schema, entry.first,
			                                  name, key_column_type(name));
			altered.insert({entry.first, StringUtil::Replace(StringUtil::Replace(name, "[", ""), "]", "")});
		}
		constraints_ddl += StringUtil::Format("ALTER TABLE %s.%s ADD CONSTRAINT %s PRIMARY KEY (%s);\n", schema,
		                                      entry.first, key_name, entry.second);
		touched.insert(entry.first);
	}

	// A string column as this build declares it: VARCHAR at its length under the UTF-8 BIN2 collation.
	auto declared_as = [&](const string &table, const string &column, int64_t length) {
		auto found = state.columns.find({table, column});
		if (found == state.columns.end()) {
			return true; // not there: nothing to declare
		}
		auto &c = found->second;
		return c.type == "varchar" && c.max_length == (length == CatalogLengths::MAX ? -1 : length) &&
		       c.collation == VARCHAR_COLLATION;
	};

	// The statistics DuckLake compares server-side. Its min/max values are the bytes DuckDB computed
	// in UTF-8 order, and a filter it pushes down is answered by the column's collation - so a
	// linguistic one prunes wrongly and drops rows from a result. BIN2 over UTF-8 is DuckDB's order.
	const vector<pair<string, string>> stats_columns = {
	    {"ducklake_file_column_stats", "min_value"},  {"ducklake_file_column_stats", "max_value"},
	    {"ducklake_table_column_stats", "min_value"}, {"ducklake_table_column_stats", "max_value"},
	    {"ducklake_file_variant_stats", "min_value"}, {"ducklake_file_variant_stats", "max_value"},
	};
	const auto stats_length = LengthOf(LengthClass::STATS, lengths);
	for (auto &entry : stats_columns) {
		if (!has_table(entry.first) || declared_as(entry.first, entry.second, stats_length)) {
			continue;
		}
		columns_ddl += StringUtil::Format("ALTER TABLE %s.%s ALTER COLUMN %s %s COLLATE %s;\n", schema, entry.first,
		                                  entry.second, VarcharOf(stats_length), VARCHAR_COLLATION);
		altered.insert(entry);
		touched.insert(entry.first);
	}

	// A partition value is bounded so it can be an index key. DuckLake declares it an unbounded
	// VARCHAR, which lands as a LOB, and a LOB can be neither an index key nor an INCLUDE - so the
	// pruning predicate below would have nothing to seek. 200 bytes of UTF-8 is far more than a
	// partition key ever is (a date, a tenant, a bucket) and well inside the 900-byte index limit;
	// a longer one is refused by the server rather than silently truncated.
	if (has_table("ducklake_file_partition_value") &&
	    !declared_as("ducklake_file_partition_value", "partition_value", 200)) {
		columns_ddl += StringUtil::Format("ALTER TABLE %s.ducklake_file_partition_value ALTER COLUMN "
		                                  "partition_value VARCHAR(200) COLLATE %s NULL;\n",
		                                  schema, VARCHAR_COLLATION);
		altered.insert({"ducklake_file_partition_value", "partition_value"});
		touched.insert("ducklake_file_partition_value");
	}

	// Everything else DuckLake declared as a string. mssql maps DuckDB's VARCHAR to NVARCHAR, which
	// is UTF-16: two bytes per character for catalog content that is paths, type names and
	// identifiers, and a linguistic case-insensitive collation over them. Neither is what this
	// catalog wants. VARCHAR under a UTF-8 collation stores the same text in half the space for that
	// content, and BIN2 is DuckDB's own comparison - DuckDB has no case-insensitive string compare,
	// so a server that does is the one behaving differently.
	//
	// Every N-type column of a `ducklake%` table, whatever DuckLake's list is this bump - a column
	// added upstream would otherwise silently keep the wrong type; declared by what it holds, at the
	// catalog's limits (specs/018), and MAX for one the list does not know. `ducklake%` in the
	// metadata schema is the extension's namespace: DuckLake creates and drops tables under that
	// prefix there by itself. Column by column because ALTER COLUMN cannot restate a whole table, and
	// nullability restated or the column silently becomes nullable. Once converted there is nothing
	// left to match - but the FIRST attach after an upgrade converts in place, and ALTER COLUMN
	// rewrites the table (specs/006 D4 measures it).
	std::map<pair<string, string>, int64_t> declared_lengths;
	for (auto &column : CatalogStringColumns()) {
		if (column.length_class != LengthClass::STATS) {
			declared_lengths[{column.table, column.column}] = LengthOf(column.length_class, lengths);
		}
	}
	for (auto &entry : state.columns) {
		auto &c = entry.second;
		if (altered.count(entry.first) || (c.type != "nvarchar" && c.type != "nchar" && c.type != "ntext")) {
			continue;
		}
		auto length = declared_lengths.find(entry.first);
		columns_ddl +=
		    StringUtil::Format("ALTER TABLE %s.%s ALTER COLUMN %s %s COLLATE %s %s;\n", schema,
		                       QuoteName(entry.first.first), QuoteName(entry.first.second),
		                       VarcharOf(length == declared_lengths.end() ? CatalogLengths::MAX : length->second),
		                       VARCHAR_COLLATION, c.nullable ? "NULL" : "NOT NULL");
		touched.insert(entry.first.first);
	}

	// The inlined deletion tables an older build created keyless, through the base's DDL (the
	// manager writes them keyed now - mssql_metadata_queries.cpp). DuckLake DELETEs from them on a
	// flush, which the mssql extension refuses without a key.
	for (auto &table : state.tables) {
		if (!StringUtil::StartsWith(table, "ducklake_inlined_delete") || state.keys.count(table)) {
			continue;
		}
		auto name = QuoteName(table);
		// NOT NULL in the first batch, the key in the second: inside one batch the constraint does not
		// see the new nullability (see below)
		columns_ddl += StringUtil::Format("ALTER TABLE %s.%s ALTER COLUMN file_id BIGINT NOT NULL; ALTER TABLE %s.%s "
		                                  "ALTER COLUMN row_id BIGINT NOT NULL; ALTER TABLE %s.%s ALTER COLUMN "
		                                  "begin_snapshot BIGINT NOT NULL;\n",
		                                  schema, name, schema, name, schema, name);
		constraints_ddl += StringUtil::Format("ALTER TABLE %s.%s ADD CONSTRAINT %s PRIMARY KEY (file_id, row_id, "
		                                      "begin_snapshot);\n",
		                                      schema, name, QuoteName("pk_" + table));
		touched.insert(table);
	}

	// Indexes on the condition almost every DuckLake read carries. Neither the postgres nor the
	// sqlite manager has indexes here at all.
	//
	// The two file tables are keyed on the whole visibility condition rather than filtered on part of
	// it. A filtered `WHERE end_snapshot IS NULL` index serves a read of the current state and, by
	// construction, nothing else: a read at an older snapshot wants the rows whose end_snapshot is
	// SET, which the filter excludes, so it falls back to scanning the table - and that scan grows
	// with the whole table rather than with the answer. Measured over 300,000 files across 1000
	// tables, half of them superseded, with the rounds alternated:
	//
	//     filtered only       current 0.001s   at an old snapshot 0.006 - 0.008s
	//     unfiltered only     current 0.001s   at an old snapshot 0.001s
	//     both                current 0.001s   at an old snapshot 0.001s
	//
	// One unfiltered index does what two do, so this replaces rather than adds. Index names are
	// unique per table, not per database, so the state is this schema's - two lakes in two schemas of
	// one database name their indexes identically.
	const vector<pair<string, string>> visibility_indexes = {
	    {"ducklake_data_file", "table_id, begin_snapshot, end_snapshot"},
	    {"ducklake_delete_file", "table_id, begin_snapshot, end_snapshot"},
	};
	for (auto &entry : visibility_indexes) {
		if (!has_table(entry.first)) {
			continue;
		}
		const auto visible = "ix_" + entry.first + "_visible";
		// the filtered index this replaces, left behind by an older build of this extension
		const auto live = "ix_" + entry.first + "_live";
		if (!has_index(visible)) {
			constraints_ddl +=
			    StringUtil::Format("CREATE INDEX %s ON %s.%s(%s);\n", visible, schema, entry.first, entry.second);
		}
		if (has_index(live)) {
			constraints_ddl += StringUtil::Format("DROP INDEX %s ON %s.%s;\n", live, schema, entry.first);
		}
	}

	// The rest keep the filtered form: their hot reads are the catalog load, which asks for the
	// current state and nothing else, and they are small enough that the difference was not worth
	// measuring either way.
	const vector<pair<string, string>> live_indexes = {
	    {"ducklake_table", "schema_id"},
	    {"ducklake_column", "table_id"},
	    {"ducklake_view", "schema_id"},
	};
	for (auto &entry : live_indexes) {
		const auto live = "ix_" + entry.first + "_live";
		if (has_table(entry.first) && !has_index(live)) {
			constraints_ddl += StringUtil::Format("CREATE INDEX %s ON %s.%s(%s) WHERE end_snapshot IS NULL;\n", live,
			                                      schema, entry.first, entry.second);
		}
	}

	// The per-column statistics a filtered read prunes with: the largest table in the catalog, a row
	// per file per column, asked for as `column_id = ? AND table_id = ?`. The primary key is
	// (data_file_id, column_id) and its leading column is not in that predicate, so nothing served
	// it. Measured over 1000 tables holding 1.23M stats rows between them, asking one table for one
	// column: 0.001s against 0.015s, and the server seeks this index rather than scanning the key.
	// The distribution is what makes it matter: with every row under a single table_id the same query
	// matches 30,000 rows, a scan is competitive, and the index measures as worthless. Keys only - the
	// min/max are long strings, and stats have no end_snapshot: they belong to a file.
	const string stats_lookup = "ix_ducklake_file_column_stats_lookup";
	if (has_table("ducklake_file_column_stats") && !has_index(stats_lookup)) {
		constraints_ddl += StringUtil::Format(
		    "CREATE INDEX %s ON %s.ducklake_file_column_stats(table_id, column_id);\n", stats_lookup, schema);
	}

	// Partition pruning, which is the whole reason to partition: DuckLake turns a filter on a
	// partition key into
	//   SELECT data_file_id FROM ducklake_file_partition_value
	//   WHERE table_id = ? AND partition_key_index = ? AND partition_value IN (...)
	// (ducklake_metadata_manager.cpp). The primary key is (data_file_id, partition_key_index) and its
	// leading column is not in that predicate, so nothing served it - the same shape that made the
	// file-column-stats index worth 15x. All three predicate columns are in the key, so the server
	// seeks and reads nothing it does not return; this is what the VARCHAR(200) above is for.
	const string partition_lookup = "ix_ducklake_file_partition_value_lookup";
	if (has_table("ducklake_file_partition_value") && !has_index(partition_lookup)) {
		constraints_ddl += StringUtil::Format(
		    "CREATE INDEX %s ON %s.ducklake_file_partition_value(table_id, partition_key_index, partition_value);\n",
		    partition_lookup, schema);
	}

	// Last, and only if everything above succeeded: the version stamp ReadCatalogMarkers reads, and
	// the limits the strings were declared with (specs/018). The stamp is what lets a catalog shaped
	// by an older build be brought up to the current shape; the state says whether to add or update.
	auto set_property = [&](const char *name, const string &value) {
		return StringUtil::Format("EXEC %s @name = N'%s', @value = N'%s', @level0type = N'SCHEMA', @level0name = %s, "
		                          "@level1type = N'TABLE', @level1name = N'ducklake_metadata';\n",
		                          state.properties.count(name) ? "sp_updateextendedproperty" : "sp_addextendedproperty",
		                          name, value, schema_literal);
	};
	constraints_ddl += set_property(SHAPE_VERSION_PROPERTY, to_string(SHAPE_VERSION));
	constraints_ddl += set_property(LIMITS_PROPERTY, lengths.Serialize());
	// the schema-level stamp one build wrote (specs/006 D4, before specs/008 moved it): a leftover that
	// would otherwise outlive any catalog dropped from under it
	if (state.schema_stamp) {
		constraints_ddl += StringUtil::Format(
		    "EXEC sp_dropextendedproperty @name = N'%s', @level0type = N'SCHEMA', @level0name = %s;\n",
		    SHAPE_VERSION_PROPERTY, schema_literal);
	}

	// Two batches: inside one, a column's new NOT NULL is not yet visible to the constraint that
	// needs it, and the server answers "cannot define PRIMARY KEY on a nullable column".
	if (!columns_ddl.empty()) {
		RunServerSide(columns_ddl, "Failed to prepare the DuckLake catalog columns for SQL Server: ");
	}
	RunServerSide(constraints_ddl, "Failed to key and index the DuckLake catalog for SQL Server: ");
	// what the preloaded cache holds of these tables predates their keys and column types; the next
	// use reads them again, on this transaction's connection (the invalidation itself is in-process)
	for (auto &table : touched) {
		InvalidateTableCache(table);
	}

	ApplyDatabaseOptions();
}

void MSSQLMetadataManager::ApplyDatabaseOptions() {
	// Every query the extension and DuckLake send this database carries its literals in the text -
	// a table name in the metadata query, `WHERE table_id = 1053` in DuckLake's own - and SQL Server
	// caches ad-hoc plans by text, so each distinct value is a plan of its own and its first
	// execution compiles it: 28-37 ms for the metadata query, measured on the plan-cache counter
	// (specs/009). A touch-each-table-once workload compiles once per table. With the option the
	// server parameterizes the literals itself and one plan serves every value: the 1000-table
	// benchmark went from 937 s to 686-698 s, the first write into each table two to three times
	// faster, the first read after an attach four times (specs/012).
	ApplyDatabaseOption("mssql_ducklake_forced_parameterization", "PARAMETERIZATION FORCED",
	                    "is_parameterization_forced = 1",
	                    "which costs a plan compile per distinct literal (specs/012)");
	// A catalog grows by thousands of rows per minute of commits, so its statistics go stale often,
	// and by default the query that finds them stale recomputes them before it runs: the first read
	// after 1000 commits into one table waited 1.2 s on six statistics of ducklake_file_column_stats
	// (300k rows) for a 9 ms statement. Asynchronous, that query runs on the old statistics and the
	// recompute happens beside it - postgres' ANALYZE is a background job too (specs/017).
	ApplyDatabaseOption("mssql_ducklake_async_statistics", "AUTO_UPDATE_STATISTICS_ASYNC ON",
	                    "is_auto_update_stats_async_on = 1",
	                    "which makes the first query after many commits wait for its statistics (specs/017)");
}

void MSSQLMetadataManager::ApplyDatabaseOption(const char *setting, const char *option, const char *already_set,
                                               const char *without_it) {
	// Database-wide options, so each has an opt-out, and best-effort, so a login that may shape the
	// schema but not alter the database - or a platform without the option, Fabric Warehouse and
	// Synapse among them - still gets a working catalog. Applied here, with the rest of the shape,
	// and not on every attach: a DBA who sets one back keeps it back.
	auto client_context = transaction.context.lock();
	if (!client_context) {
		throw InternalException("MSSQLMetadataManager: the client context is gone");
	}
	Value wanted;
	if (client_context->TryGetCurrentSetting(setting, wanted) && !wanted.GetValue<bool>()) {
		return;
	}
	auto statement = StringUtil::Format("ALTER DATABASE CURRENT SET %s;", option);
	// Only when it is not set yet: ALTER DATABASE ... SET flushes the database's plan cache even when
	// it changes nothing, and every new catalog in the database shapes - each would throw away the
	// plans of every other catalog there.
	auto guarded = StringUtil::Format(
	    "IF NOT EXISTS (SELECT 1 FROM sys.databases WHERE database_id = DB_ID() AND %s) %s", already_set, statement);
	try {
		// ALTER DATABASE is refused inside a transaction; on its own connection it is also online -
		// measured against a session holding uncommitted DDL and a row lock in this database, it
		// completed in a second.
		RunServerSideOutsideTransaction(guarded,
		                                StringUtil::Format("Failed to set %s on the catalog's database: ", option));
	} catch (std::exception &ex) {
		DUCKDB_LOG_WARNING(*client_context,
		                   StringUtil::Format("mssql_ducklake: the catalog's database goes without %s, %s. To apply "
		                                      "it by hand: %s %s",
		                                      option, without_it, statement, ex.what()));
	}
}

void MSSQLMetadataManager::InitializeDuckLake(bool has_explicit_schema, DuckLakeEncryption encryption) {
	// DuckLake's own DDL first, through duckdb - it owns the shape of its catalog, and reproducing
	// it here would be a copy to re-audit on every submodule bump. Then ours, at once.
	DuckLakeMetadataManager::InitializeDuckLake(has_explicit_schema, encryption);
	EnsureCatalogShape();
}

} // namespace duckdb
