#include "mssql_metadata_manager.hpp"
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

bool MSSQLMetadataManager::CatalogShapeIsCurrent() {
	// One cheap question instead of two batches of DDL. Attaching an existing catalog is on the hot
	// path - every transaction pays for whatever happens here - and the shaping is only ever needed
	// once per catalog per version of this extension.
	//
	// A version rather than the presence of one constraint. The marker used to be the last key the
	// shaping adds, which answered "some build of this extension shaped this catalog" - so a catalog
	// shaped by an older one, whose column types and indexes are not what the current build wants,
	// read as current and was left alone. The stamp is written last, after the DDL that earns it.
	auto &connection = transaction.GetConnection();
	// the inner statement travels inside a duckdb string literal, so each of its own quotes is
	// doubled once - the same shape the collation probe above uses
	auto schema_name = StringUtil::Replace(transaction.GetCatalog().MetadataSchemaName(), "'", "''''");
	// The stamp sits on ducklake_metadata - the catalog's own anchor table, the one DuckLake probes to
	// decide whether a catalog exists - and not on the schema. It was on the schema for one day, and
	// that was a regression: a catalog whose tables were dropped and recreated (with our shaping
	// failing partway, as it did behind a dying session's locks) kept the schema's stamp, every later
	// attach trusted it, and the catalog stayed without keys until the first UPDATE failed. A stamp on
	// the table dies with the table, so a recreated catalog is shaped again - the self-healing the
	// old constraint marker had by construction.
	auto result = connection.Query(StringUtil::Format(
	    "SELECT shape FROM mssql_scan(%s, 'SELECT TRY_CAST(CAST(value AS VARCHAR(32)) AS BIGINT) AS shape "
	    "FROM sys.extended_properties WHERE class = 1 "
	    "AND major_id = OBJECT_ID(QUOTENAME(''%s'') + ''.ducklake_metadata'') AND minor_id = 0 AND name = ''%s''')",
	    CatalogLiteral(), schema_name, SHAPE_VERSION_PROPERTY));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to inspect the DuckLake catalog on SQL Server: ");
	}
	auto row = result->Fetch();
	if (!row || row->size() == 0 || row->GetValue(0, 0).IsNull()) {
		return false;
	}
	return row->GetValue(0, 0).GetValue<int64_t>() >= SHAPE_VERSION;
}

void MSSQLMetadataManager::EnsureCatalogShape() {
	const string schema = SchemaIdentifier();
	const string schema_literal = DuckLakeUtil::SQLLiteralToString(transaction.GetCatalog().MetadataSchemaName());

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
	    {"ducklake_schema_versions", "begin_snapshot, schema_version"},
	};
	// The tag tables and the metadata table key on a name rather than an id. A primary key cannot be
	// over MAX, so those are capped - 200 bytes of UTF-8 is a long name and well inside the
	// 900-byte index limit.
	auto key_column_type = [&](const string &name) {
		if (name == "[key]" || name == "variant_path") {
			return StringUtil::Format("VARCHAR(200) COLLATE %s", VARCHAR_COLLATION);
		}
		return string("BIGINT");
	};

	string columns_ddl;
	string constraints_ddl;
	for (auto &entry : keys) {
		for (auto &column : StringUtil::Split(entry.second, ',')) {
			auto name = column;
			StringUtil::Trim(name);
			// a key column has to be NOT NULL, and DuckLake declares none of them so
			columns_ddl += StringUtil::Format("ALTER TABLE %s.%s ALTER COLUMN %s %s NOT NULL;\n", schema, entry.first,
			                                  name, key_column_type(name));
		}
		// idempotent, so that an existing catalog can be brought up to shape on attach and a run
		// that failed partway can be resumed
		constraints_ddl += StringUtil::Format(
		    "IF NOT EXISTS (SELECT 1 FROM sys.key_constraints WHERE name = 'pk_%s' AND schema_id = SCHEMA_ID(%s)) "
		    "ALTER TABLE %s.%s ADD CONSTRAINT pk_%s PRIMARY KEY (%s);\n",
		    entry.first, schema_literal, schema, entry.first, entry.first, entry.second);
	}

	// The statistics DuckLake compares server-side. Its min/max values are the bytes DuckDB computed
	// in UTF-8 order, and a filter it pushes down is answered by the column's collation - so a
	// linguistic one prunes wrongly and drops rows from a result. BIN2 over UTF-8 is DuckDB's order.
	const vector<pair<string, string>> stats_columns = {
	    {"ducklake_file_column_stats", "min_value"},  {"ducklake_file_column_stats", "max_value"},
	    {"ducklake_table_column_stats", "min_value"}, {"ducklake_table_column_stats", "max_value"},
	    {"ducklake_file_variant_stats", "min_value"}, {"ducklake_file_variant_stats", "max_value"},
	};
	for (auto &entry : stats_columns) {
		columns_ddl += StringUtil::Format("ALTER TABLE %s.%s ALTER COLUMN %s VARCHAR(MAX) COLLATE %s;\n", schema,
		                                  entry.first, entry.second, VARCHAR_COLLATION);
	}

	// A partition value is bounded so it can be an index key. DuckLake declares it an unbounded
	// VARCHAR, which lands as a LOB, and a LOB can be neither an index key nor an INCLUDE - so the
	// pruning predicate below would have nothing to seek. 200 bytes of UTF-8 is far more than a
	// partition key ever is (a date, a tenant, a bucket) and well inside the 900-byte index limit;
	// a longer one is refused by the server rather than silently truncated.
	columns_ddl += StringUtil::Format("ALTER TABLE %s.ducklake_file_partition_value ALTER COLUMN partition_value "
	                                  "VARCHAR(200) COLLATE %s NULL;\n",
	                                  schema, VARCHAR_COLLATION);

	// Everything else DuckLake declared as a string. mssql maps DuckDB's VARCHAR to NVARCHAR, which
	// is UTF-16: two bytes per character for catalog content that is paths, type names and
	// identifiers, and a linguistic case-insensitive collation over them. Neither is what this
	// catalog wants. VARCHAR under a UTF-8 collation stores the same text in half the space for that
	// content, and BIN2 is DuckDB's own comparison - DuckDB has no case-insensitive string compare,
	// so a server that does is the one behaving differently.
	//
	// Generated from sys.columns rather than listed here: the list is DuckLake's, it moves with every
	// submodule bump, and a column added upstream would otherwise silently keep the wrong type. Once
	// converted the sweep matches nothing, so it costs a single statement on every later attach - but
	// the FIRST attach after an upgrade converts in place, and ALTER COLUMN rewrites the table, so
	// that one attach walks the whole catalog (specs/006 D4 measures it).
	//
	// `ducklake%` in the metadata schema is the extension's namespace, not a guess: DuckLake creates
	// and drops tables under that prefix there by itself (every `ducklake_inlined_data_<t>_<v>`), so
	// a table of someone else's answering to it would already be colliding with DuckLake. The scope
	// is deliberately the same one the integration suite's reset uses.
	//
	// Column-by-column because ALTER COLUMN cannot restate a whole table, and nullability has to be
	// restated or the column silently becomes nullable.
	columns_ddl += StringUtil::Format(R"(
DECLARE @widen NVARCHAR(MAX) = N'';
SELECT @widen += N'ALTER TABLE ' + QUOTENAME(sch.name) + N'.' + QUOTENAME(t.name)
               + N' ALTER COLUMN ' + QUOTENAME(c.name) + N' VARCHAR(MAX) COLLATE %s'
               + CASE WHEN c.is_nullable = 0 THEN N' NOT NULL' ELSE N' NULL' END + N';'
FROM sys.columns c
JOIN sys.tables t ON t.object_id = c.object_id
JOIN sys.schemas sch ON sch.schema_id = t.schema_id
JOIN sys.types ty ON ty.user_type_id = c.user_type_id
WHERE sch.name = %s AND t.name LIKE 'ducklake%%' AND ty.name IN ('nvarchar', 'nchar', 'ntext');
EXEC sp_executesql @widen;
)",
	                                  VARCHAR_COLLATION, schema_literal);

	// The inlined deletion tables an older build created keyless, through the base's DDL (the
	// manager writes them keyed now - mssql_metadata_queries.cpp). DuckLake DELETEs from them on a
	// flush, which the mssql extension refuses without a key. Same dynamic form as the widening
	// above: the tables are per lake table and the shaping cannot name them.
	constraints_ddl += StringUtil::Format(R"(
DECLARE @key NVARCHAR(MAX) = N'';
SELECT @key += N'ALTER TABLE ' + QUOTENAME(sch.name) + N'.' + QUOTENAME(t.name)
             + N' ALTER COLUMN file_id BIGINT NOT NULL; ALTER TABLE ' + QUOTENAME(sch.name) + N'.' + QUOTENAME(t.name)
             + N' ALTER COLUMN row_id BIGINT NOT NULL; ALTER TABLE ' + QUOTENAME(sch.name) + N'.' + QUOTENAME(t.name)
             + N' ALTER COLUMN begin_snapshot BIGINT NOT NULL; ALTER TABLE ' + QUOTENAME(sch.name) + N'.' + QUOTENAME(t.name)
             + N' ADD CONSTRAINT ' + QUOTENAME('pk_' + t.name) + N' PRIMARY KEY (file_id, row_id, begin_snapshot);'
FROM sys.tables t
JOIN sys.schemas sch ON sch.schema_id = t.schema_id
WHERE sch.name = %s AND t.name LIKE 'ducklake_inlined_delete%%'
  AND NOT EXISTS (SELECT 1 FROM sys.key_constraints kc WHERE kc.parent_object_id = t.object_id AND kc.type = 'PK');
EXEC sp_executesql @key;
)",
	                                      schema_literal);

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
	// One unfiltered index does what two do, so this replaces rather than adds. Time travel is the
	// obvious beneficiary; so is every maintenance function that walks history.
	const vector<pair<string, string>> visibility_indexes = {
	    {"ducklake_data_file", "table_id, begin_snapshot, end_snapshot"},
	    {"ducklake_delete_file", "table_id, begin_snapshot, end_snapshot"},
	};
	// Every check below is scoped to this catalog's schema. `sys.indexes.name` is unique per table,
	// not per database, so two lakes in two schemas of one database name their indexes identically -
	// and an unscoped existence check would let the second one skip creating indexes it has not got.
	// The primary keys above are scoped for the same reason.
	auto index_exists = [&](const string &index_name) {
		return StringUtil::Format("SELECT 1 FROM sys.indexes i JOIN sys.objects o ON o.object_id = i.object_id "
		                          "WHERE i.name = '%s' AND o.schema_id = SCHEMA_ID(%s)",
		                          index_name, schema_literal);
	};
	for (auto &entry : visibility_indexes) {
		const auto visible = "ix_" + entry.first + "_visible";
		constraints_ddl += StringUtil::Format("IF NOT EXISTS (%s) CREATE INDEX %s ON %s.%s(%s);\n",
		                                      index_exists(visible), visible, schema, entry.first, entry.second);
		// the filtered index this replaces, left behind by an older build of this extension
		const auto live = "ix_" + entry.first + "_live";
		constraints_ddl += StringUtil::Format("IF EXISTS (%s) DROP INDEX %s ON %s.%s;\n", index_exists(live), live,
		                                      schema, entry.first);
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
		constraints_ddl +=
		    StringUtil::Format("IF NOT EXISTS (%s) CREATE INDEX %s ON %s.%s(%s) WHERE end_snapshot IS NULL;\n",
		                       index_exists(live), live, schema, entry.first, entry.second);
	}

	// The per-column statistics a filtered read prunes with: the largest table in the catalog, a row
	// per file per column, asked for as `column_id = ? AND table_id = ?`. The primary key is
	// (data_file_id, column_id) and its leading column is not in that predicate, so nothing served
	// it. Measured over 1000 tables holding 1.23M stats rows between them, asking one table for one
	// column: 0.001s against 0.015s, and the server seeks this index rather than scanning the key.
	//
	// The distribution is what makes it matter, and measuring it wrong is easy: with every row under
	// a single table_id the same query matches 30,000 rows, a scan is competitive, and the index
	// measures as worthless. A real catalog spreads its rows over its tables.
	//
	// Keys only, no INCLUDE. min_value and max_value are VARCHAR(MAX) - DuckLake declares them
	// without a length and bounds nothing it writes - and a MAX column is a LOB, so it can be
	// neither an index key nor something worth duplicating into one. No filtered form either: stats
	// have no end_snapshot, they belong to a file and the file is what expires.
	const string stats_lookup = "ix_ducklake_file_column_stats_lookup";
	constraints_ddl += StringUtil::Format(
	    "IF NOT EXISTS (%s) CREATE INDEX %s ON %s.ducklake_file_column_stats(table_id, column_id);\n",
	    index_exists(stats_lookup), stats_lookup, schema);

	// Partition pruning, which is the whole reason to partition: DuckLake turns a filter on a
	// partition key into
	//   SELECT data_file_id FROM ducklake_file_partition_value
	//   WHERE table_id = ? AND partition_key_index = ? AND partition_value IN (...)
	// (ducklake_metadata_manager.cpp). The primary key is (data_file_id, partition_key_index) and its
	// leading column is not in that predicate, so nothing served it - the same shape that made the
	// file-column-stats index worth 15x. All three predicate columns are in the key, so the server
	// seeks and reads nothing it does not return; this is what the VARCHAR(200) above is for.
	const string partition_lookup = "ix_ducklake_file_partition_value_lookup";
	constraints_ddl += StringUtil::Format("IF NOT EXISTS (%s) CREATE INDEX %s ON "
	                                      "%s.ducklake_file_partition_value(table_id, partition_key_index, "
	                                      "partition_value);\n",
	                                      index_exists(partition_lookup), partition_lookup, schema);

	// Last, and only if everything above succeeded: the version stamp CatalogShapeIsCurrent reads.
	// It is what lets a catalog shaped by an older build of this extension be brought up to the
	// current shape - the DDL is all idempotent, so the stamp is the only thing that decides whether
	// it is worth running at all.
	constraints_ddl +=
	    StringUtil::Format(R"(
IF EXISTS (SELECT 1 FROM sys.extended_properties WHERE class = 1 AND major_id = OBJECT_ID(QUOTENAME(%s) + '.ducklake_metadata') AND minor_id = 0 AND name = '%s')
    EXEC sp_updateextendedproperty @name = N'%s', @value = N'%d', @level0type = N'SCHEMA', @level0name = %s, @level1type = N'TABLE', @level1name = N'ducklake_metadata';
ELSE
    EXEC sp_addextendedproperty @name = N'%s', @value = N'%d', @level0type = N'SCHEMA', @level0name = %s, @level1type = N'TABLE', @level1name = N'ducklake_metadata';
-- the schema-level stamp one build wrote (specs/006 D4, before specs/008 moved it): a leftover that
-- would otherwise outlive any catalog dropped from under it
IF EXISTS (SELECT 1 FROM sys.extended_properties WHERE class = 3 AND major_id = SCHEMA_ID(%s) AND name = '%s')
    EXEC sp_dropextendedproperty @name = N'%s', @level0type = N'SCHEMA', @level0name = %s;
)",
	                       schema_literal, SHAPE_VERSION_PROPERTY, SHAPE_VERSION_PROPERTY, SHAPE_VERSION,
	                       schema_literal, SHAPE_VERSION_PROPERTY, SHAPE_VERSION, schema_literal, schema_literal,
	                       SHAPE_VERSION_PROPERTY, SHAPE_VERSION_PROPERTY, schema_literal);

	// Two batches: inside one, a column's new NOT NULL is not yet visible to the constraint that
	// needs it, and the server answers "cannot define PRIMARY KEY on a nullable column".
	RunServerSide(columns_ddl, "Failed to prepare the DuckLake catalog columns for SQL Server: ");
	RunServerSide(constraints_ddl, "Failed to key and index the DuckLake catalog for SQL Server: ");

	ApplyForcedParameterization();
}

void MSSQLMetadataManager::ApplyForcedParameterization() {
	// Every query the extension and DuckLake send this database carries its literals in the text -
	// a table name in the metadata query, `WHERE table_id = 1053` in DuckLake's own - and SQL Server
	// caches ad-hoc plans by text, so each distinct value is a plan of its own and its first
	// execution compiles it: 28-37 ms for the metadata query, measured on the plan-cache counter
	// (specs/009). A touch-each-table-once workload compiles once per table. With the option the
	// server parameterizes the literals itself and one plan serves every value: the 1000-table
	// benchmark went from 937 s to 686-698 s, the first write into each table two to three times
	// faster, the first read after an attach four times (specs/012).
	//
	// A database-wide option, so it has an opt-out, and best-effort, so a login that may shape the
	// schema but not alter the database - or a platform without the option, Fabric Warehouse and
	// Synapse among them - still gets a working catalog. Applied here, with the rest of the shape,
	// and not on every attach: a DBA who sets it back keeps it back.
	auto client_context = transaction.context.lock();
	if (!client_context) {
		throw InternalException("MSSQLMetadataManager: the client context is gone");
	}
	Value wanted;
	if (client_context->TryGetCurrentSetting("mssql_ducklake_forced_parameterization", wanted) &&
	    !wanted.GetValue<bool>()) {
		return;
	}
	try {
		// ALTER DATABASE is refused inside a transaction; on its own connection it is also online -
		// measured against a session holding uncommitted DDL and a row lock in this database, it
		// completed in a second.
		RunServerSideOutsideTransaction("ALTER DATABASE CURRENT SET PARAMETERIZATION FORCED;",
		                                "Failed to set PARAMETERIZATION FORCED on the catalog's database: ");
	} catch (std::exception &ex) {
		DUCKDB_LOG_WARNING(*client_context,
		                   StringUtil::Format("mssql_ducklake: the catalog's database keeps simple parameterization, "
		                                      "which costs a plan compile per distinct literal (specs/012). To apply "
		                                      "it by hand: ALTER DATABASE CURRENT SET PARAMETERIZATION FORCED. %s",
		                                      ex.what()));
	}
}

void MSSQLMetadataManager::InitializeDuckLake(bool has_explicit_schema, DuckLakeEncryption encryption) {
	auto &connection = transaction.GetConnection();
	// The catalog's string columns are stored with a UTF-8 collation, so the server has to have one.
	// Asking for the collation itself rather than the version: Azure SQL Database reports major
	// version 12 while supporting it, and the version was only ever a proxy for this question.
	auto probe = connection.Query(StringUtil::Format(
	    "SELECT collations FROM mssql_scan(%s, 'SELECT COUNT(*) AS collations FROM sys.fn_helpcollations() "
	    "WHERE name = ''%s''')",
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

	// DuckLake's own DDL first, through duckdb - it owns the shape of its catalog, and reproducing
	// it here would be a copy to re-audit on every submodule bump.
	DuckLakeMetadataManager::InitializeDuckLake(has_explicit_schema, encryption);
	EnsureCatalogShape();
}

} // namespace duckdb
