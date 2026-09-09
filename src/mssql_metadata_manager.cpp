#include "mssql_metadata_manager.hpp"

#include "common/ducklake_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/main/database.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

MSSQLMetadataManager::MSSQLMetadataManager(DuckLakeTransaction &transaction) : DuckLakeMetadataManager(transaction) {
}

namespace {

//! Is this offset the start of a whole word (so `true` in `construed` is not a match)?
bool IsWordBoundary(const string &sql, idx_t pos, idx_t len) {
	auto is_word_char = [](char c) {
		return StringUtil::CharacterIsAlphaNumeric(c) || c == '_';
	};
	if (pos > 0 && is_word_char(sql[pos - 1])) {
		return false;
	}
	const idx_t after = pos + len;
	return after >= sql.size() || !is_word_char(sql[after]);
}

bool MatchesAt(const string &sql, idx_t pos, const char *needle) {
	return sql.compare(pos, strlen(needle), needle) == 0;
}

//! The identifier of the table a `CREATE TABLE IF NOT EXISTS <identifier>(` names, as written -
//! ducklake produces a quoted, schema-qualified name here.
string ReadTableIdentifier(const string &sql, idx_t pos, idx_t &out_end) {
	idx_t start = pos;
	while (start < sql.size() && StringUtil::CharacterIsSpace(sql[start])) {
		start++;
	}
	idx_t end = start;
	bool in_quotes = false;
	while (end < sql.size()) {
		const char c = sql[end];
		if (c == '"') {
			in_quotes = !in_quotes;
		} else if (!in_quotes && (c == '(' || StringUtil::CharacterIsSpace(c))) {
			break;
		}
		end++;
	}
	out_end = end;
	return sql.substr(start, end - start);
}

//! Quote a comma-separated column list the way T-SQL wants it, so a name that collides with a
//! reserved word (ducklake has a `key` column) survives.
string QuoteColumnList(const string &columns) {
	string result;
	for (auto &column : StringUtil::Split(columns, ',')) {
		auto name = column;
		StringUtil::Trim(name);
		if (name.empty()) {
			continue;
		}
		if (!result.empty()) {
			result += ", ";
		}
		if (name.front() == '"') {
			result += name;
		} else {
			result += "\"" + StringUtil::Replace(name, "\"", "\"\"") + "\"";
		}
	}
	return result;
}

//! Skip from an opening parenthesis to the one that closes it, stepping over string literals.
//! Returns npos when the batch is unbalanced, which means we did not understand it.
idx_t FindMatchingParen(const string &sql, idx_t open_paren) {
	idx_t depth = 0;
	for (idx_t i = open_paren; i < sql.size(); i++) {
		const char c = sql[i];
		if (c == '\'' || c == '"') {
			const char quote = c;
			i++;
			while (i < sql.size()) {
				if (sql[i] == quote) {
					if (i + 1 < sql.size() && sql[i + 1] == quote) {
						i++;
					} else {
						break;
					}
				}
				i++;
			}
			continue;
		}
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			depth--;
			if (depth == 0) {
				return i;
			}
		}
	}
	return DConstants::INVALID_INDEX;
}

idx_t SkipSpaces(const string &sql, idx_t pos) {
	while (pos < sql.size() && StringUtil::CharacterIsSpace(sql[pos])) {
		pos++;
	}
	return pos;
}

} // namespace

MSSQLMetadataManager::TranspiledBatch MSSQLMetadataManager::TranspileBatch(const string &query) {
	TranspiledBatch batch;
	string &result = batch.sql;
	result.reserve(query.size() + query.size() / 8);

	for (idx_t i = 0; i < query.size();) {
		const char c = query[i];

		// Data, not code: copy string literals and quoted identifiers through untouched, so a value
		// that happens to contain NOW() or the word `true` survives. '' and "" escape themselves.
		if (c == '\'' || c == '"') {
			const char quote = c;
			// A bare 'text' literal is parsed in the database's collation code page, so anything
			// outside it becomes '?' before it ever reaches a UTF-8 column - silently. N'text' is
			// parsed as Unicode and converts losslessly into one. The database default is often a
			// legacy CP1252 collation, so this is not a corner case.
			if (quote == '\'' && !(i > 0 && (query[i - 1] == 'N' || query[i - 1] == 'n'))) {
				result += 'N';
			}
			result += quote;
			i++;
			while (i < query.size()) {
				if (query[i] == quote) {
					if (i + 1 < query.size() && query[i + 1] == quote) {
						result.append(2, quote);
						i += 2;
						continue;
					}
					result += quote;
					i++;
					break;
				}
				result += query[i++];
			}
			continue;
		}

		// NOW() - the commit timestamp of a snapshot, and of a scheduled deletion
		if (MatchesAt(query, i, "NOW()")) {
			result += "SYSDATETIMEOFFSET()";
			i += 5;
			continue;
		}
		// boolean literals: SQL Server has BIT, and no `true`/`false` keywords
		if (MatchesAt(query, i, "true") && IsWordBoundary(query, i, 4)) {
			result += "1";
			i += 4;
			continue;
		}
		if (MatchesAt(query, i, "false") && IsWordBoundary(query, i, 5)) {
			result += "0";
			i += 5;
			continue;
		}
		// the batch casts its boolean columns by DuckDB's type name
		if (MatchesAt(query, i, "BOOLEAN") && IsWordBoundary(query, i, 7)) {
			result += "BIT";
			i += 7;
			continue;
		}
		// CREATE TABLE IF NOT EXISTS x(...) -> IF OBJECT_ID('x') IS NULL CREATE TABLE x(...)
		if (MatchesAt(query, i, "CREATE TABLE IF NOT EXISTS") && IsWordBoundary(query, i, 6)) {
			idx_t name_end;
			auto identifier = ReadTableIdentifier(query, i + strlen("CREATE TABLE IF NOT EXISTS"), name_end);
			// OBJECT_ID takes the name as a string, so the identifier quotes are dropped from it
			auto object_name = StringUtil::Replace(StringUtil::Replace(identifier, "\"", ""), "'", "''");
			result += StringUtil::Format("IF OBJECT_ID('%s') IS NULL CREATE TABLE %s", object_name, identifier);
			batch.changes_schema = true;
			i = name_end;
			continue;
		}
		if (MatchesAt(query, i, "DROP TABLE") && IsWordBoundary(query, i, 4)) {
			// valid T-SQL as generated (2016+); the cache still has to hear about it
			batch.changes_schema = true;
			result += c;
			i++;
			continue;
		}
		// WITH cte(a, b) AS (VALUES ...) is DuckDB's; T-SQL needs the VALUES list to be a derived
		// table with an alias. The column list is right there, so it names the alias too.
		if (MatchesAt(query, i, "WITH") && IsWordBoundary(query, i, 4)) {
			idx_t pos = SkipSpaces(query, i + 4);
			const idx_t name_start = pos;
			while (pos < query.size() && (StringUtil::CharacterIsAlphaNumeric(query[pos]) || query[pos] == '_')) {
				pos++;
			}
			const string cte_name = query.substr(name_start, pos - name_start);
			pos = SkipSpaces(query, pos);
			if (!cte_name.empty() && pos < query.size() && query[pos] == '(') {
				const idx_t columns_end = FindMatchingParen(query, pos);
				if (columns_end != DConstants::INVALID_INDEX) {
					const string columns = query.substr(pos + 1, columns_end - pos - 1);
					idx_t after = SkipSpaces(query, columns_end + 1);
					if (MatchesAt(query, after, "AS") && IsWordBoundary(query, after, 2)) {
						after = SkipSpaces(query, after + 2);
						if (after < query.size() && query[after] == '(') {
							const idx_t body_end = FindMatchingParen(query, after);
							const idx_t body_start = SkipSpaces(query, after + 1);
							if (body_end != DConstants::INVALID_INDEX && MatchesAt(query, body_start, "VALUES")) {
								// the body is code too - it carries the booleans this batch writes, so it
								// goes through the scanner rather than being copied verbatim
								auto values = TranspileBatch(query.substr(body_start, body_end - body_start));
								batch.changes_schema |= values.changes_schema;
								const string quoted = QuoteColumnList(columns);
								result += StringUtil::Format("WITH %s(%s) AS (SELECT * FROM (%s) AS __v(%s))", cte_name,
								                             quoted, values.sql, quoted);
								i = body_end + 1;
								continue;
							}
						}
					}
				}
			}
		}

		result += c;
		i++;
	}
	return batch;
}

bool MSSQLMetadataManager::TypeIsNativelySupported(const LogicalType &type) {
	switch (type.id()) {
	// SQL Server has no NaN and no infinities, so a float column cannot round trip
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	// DATETIME2(7) is 100ns, so nanoseconds are lossy
	case LogicalTypeId::TIMESTAMP_NS:
	// wider than DECIMAL(38, 0), the widest exact numeric SQL Server has
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	// no server-side equivalent; DuckLake's canonical text is the storage
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::BIT:
	case LogicalTypeId::ENUM:
	case LogicalTypeId::VARIANT:
	case LogicalTypeId::GEOMETRY:
	// nested types are stored as text by DuckLake itself
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::MAP:
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY:
	case LogicalTypeId::UNION:
		return false;
	default:
		return true;
	}
}

string MSSQLMetadataManager::GetColumnTypeInternal(const LogicalType &column_type) {
	switch (column_type.id()) {
	case LogicalTypeId::BOOLEAN:
		return "BIT";
	// T-SQL's TINYINT is unsigned, so the signed one needs a wider column and ours fits it exactly
	case LogicalTypeId::TINYINT:
		return "SMALLINT";
	case LogicalTypeId::UTINYINT:
		return "TINYINT";
	case LogicalTypeId::SMALLINT:
		return "SMALLINT";
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::INTEGER:
		return "INT";
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::BIGINT:
		return "BIGINT";
	case LogicalTypeId::DECIMAL:
		return StringUtil::Format("DECIMAL(%d, %d)", DecimalType::GetWidth(column_type),
		                          DecimalType::GetScale(column_type));
	case LogicalTypeId::BLOB:
		return "VARBINARY(MAX)";
	case LogicalTypeId::DATE:
		return "DATE";
	case LogicalTypeId::TIME:
		return "TIME(6)";
	case LogicalTypeId::TIMESTAMP:
		return "DATETIME2(6)";
	case LogicalTypeId::TIMESTAMP_MS:
		return "DATETIME2(3)";
	case LogicalTypeId::TIMESTAMP_SEC:
		return "DATETIME2(0)";
	case LogicalTypeId::TIMESTAMP_TZ:
		return "DATETIMEOFFSET(6)";
	case LogicalTypeId::UUID:
		return "UNIQUEIDENTIFIER";
	default:
		// Everything else - the types above that are not natively supported, and DuckLake's own
		// text storage for nested values. MAX rather than a bound: DuckLake states no length, and a
		// bare VARCHAR means VARCHAR(1) in T-SQL. The collation is explicit rather than inherited
		// from the database, whose own is often a legacy CI_AS one.
		return StringUtil::Format("VARCHAR(MAX) COLLATE %s", VARCHAR_COLLATION);
	}
}

void MSSQLMetadataManager::SubstitutePassthroughPlaceholders(DuckLakeSnapshot snapshot, string &query) const {
	auto &commit_info = transaction.GetCommitInfo();
	query = StringUtil::Replace(query, "{SNAPSHOT_ID}", to_string(snapshot.snapshot_id));
	query = StringUtil::Replace(query, "{SCHEMA_VERSION}", to_string(snapshot.schema_version));
	query = StringUtil::Replace(query, "{NEXT_CATALOG_ID}", to_string(snapshot.next_catalog_id));
	query = StringUtil::Replace(query, "{NEXT_FILE_ID}", to_string(snapshot.next_file_id));
	query = StringUtil::Replace(query, "{AUTHOR}", commit_info.author.ToSQLString());
	query = StringUtil::Replace(query, "{COMMIT_MESSAGE}", commit_info.commit_message.ToSQLString());
	query = StringUtil::Replace(query, "{COMMIT_EXTRA_INFO}", commit_info.commit_extra_info.ToSQLString());

	auto &ducklake_catalog = transaction.GetCatalog();
	auto catalog_literal = DuckLakeUtil::SQLLiteralToString(ducklake_catalog.MetadataDatabaseName());
	auto catalog_identifier = DuckLakeUtil::SQLIdentifierToString(ducklake_catalog.MetadataDatabaseName());
	auto schema_identifier = DuckLakeUtil::SQLIdentifierToString(ducklake_catalog.MetadataSchemaName());
	auto schema_identifier_escaped = StringUtil::Replace(schema_identifier, "'", "''");
	auto schema_literal = DuckLakeUtil::SQLLiteralToString(ducklake_catalog.MetadataSchemaName());
	auto metadata_path = DuckLakeUtil::SQLLiteralToString(ducklake_catalog.MetadataPath());
	auto data_path = DuckLakeUtil::SQLLiteralToString(ducklake_catalog.DataPath());

	query = StringUtil::Replace(query, "{METADATA_CATALOG_NAME_LITERAL}", catalog_literal);
	query = StringUtil::Replace(query, "{METADATA_CATALOG_NAME_IDENTIFIER}", catalog_identifier);
	query = StringUtil::Replace(query, "{METADATA_SCHEMA_NAME_LITERAL}", schema_literal);
	// the schema alone: this SQL runs on the server, where the attached catalog is not a prefix
	query = StringUtil::Replace(query, "{METADATA_CATALOG}", schema_identifier);
	query = StringUtil::Replace(query, "{METADATA_SCHEMA_ESCAPED}", schema_identifier_escaped);
	query = StringUtil::Replace(query, "{METADATA_PATH}", metadata_path);
	query = StringUtil::Replace(query, "{DATA_PATH}", data_path);
}

unique_ptr<QueryResult> MSSQLMetadataManager::Execute(DuckLakeSnapshot snapshot, string &query) {
	SubstitutePassthroughPlaceholders(snapshot, query);
	auto batch = TranspileBatch(query);

	auto &ducklake_catalog = transaction.GetCatalog();
	auto catalog_literal = DuckLakeUtil::SQLLiteralToString(ducklake_catalog.MetadataDatabaseName());
	auto &connection = transaction.GetConnection();
	auto result =
	    connection.Query(StringUtil::Format("SELECT mssql_exec(%s, %s)", catalog_literal, SQLString(batch.sql)));
	if (result->HasError() || !batch.changes_schema) {
		return result;
	}
	// The batch created or dropped a table (an inlined data or deletion table). The mssql extension
	// caches catalog metadata and cannot see a change made behind its back through mssql_exec, so
	// the reads that follow in this session would miss the table entirely - a silently stale answer
	// rather than an error. Its own `mssql_exec_invalidate_cache` setting does this, but it is
	// global and the user's to set; point invalidation is ours to call.
	auto invalidate = connection.Query(StringUtil::Format("SELECT mssql_invalidate_cache(%s)", catalog_literal));
	if (invalidate->HasError()) {
		invalidate->GetErrorObject().Throw("Failed to refresh the SQL Server catalog cache after DDL: ");
	}
	return result;
}

} // namespace duckdb
