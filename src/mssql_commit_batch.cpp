#include "mssql_metadata_manager.hpp"
#include "mssql_metadata_internal.hpp"

#include "common/ducklake_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_inlined_data.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// The commit batch as our T-SQL, statement by known statement (specs/014)
//===--------------------------------------------------------------------===//
//
// DuckLake assembles a commit as one string of DuckDB SQL and hands it to Execute. Run through the
// attached catalog that is one round trip per statement through the mssql extension's DML
// operators, and a catalog-sized scan for the column-stats UPDATE. What the batch contains is a
// closed list - captured off every write DuckLake can make (design 003): INSERTs into catalog
// tables whose column types are DuckLake's DDL, UPDATEs and DELETEs with numeric predicates, four
// CTE updates, DROP TABLE IF EXISTS, one DDL (specs/006 D5b) - plus the INSERT of a user's inlined
// rows, whose values are the user's. Each of the former is recognised exactly and sent as the
// manager's own T-SQL, contiguous runs in one mssql_exec call on the transaction's connection; the
// latter and anything not on the list go to the base as they are, in order. Nothing is
// translated: a statement matches to the character or is not touched.

namespace {

//! The five literal kinds DuckLake writes into catalog statements, and NOW().
enum class LiteralKind : uint8_t { NUMBER, STRING, NULL_VALUE, BOOLEAN, NOW };

struct Literal {
	LiteralKind kind;
	//! the number's text, the string's inner text with its '' escaping kept, or "true"/"false"
	string text;
};

//! The column kinds of every catalog table, in DDL order (ducklake_metadata_manager.cpp:194-221
//! and the columns its migrations added): n BIGINT, s VARCHAR, b BOOLEAN, t TIMESTAMPTZ, u UUID.
//! A tuple is rendered column by column against this, and a table that is not here - or a tuple
//! of another width - is not rewritten.
//!
//! Both formats are supported (specs/015 R1), and they differ only at the end of five tables: format
//! 1.1 appends `row_group_count` to the data and delete files, `min_is_exact`/`max_is_exact` to both
//! stats tables and `parent_schema_id` to schemas, and adds `ducklake_view_column_tag`. A migrated
//! catalog gets those columns by `ALTER … ADD`, which appends - so the 1.0 layout is the 1.1 one with
//! the tail cut, and that is how it is derived rather than written out twice.
const unordered_map<string, string> &CatalogColumnsV1_1() {
	static const unordered_map<string, string> columns = {
	    {"ducklake_metadata", "sssn"},
	    {"ducklake_snapshot", "ntnnn"},
	    {"ducklake_snapshot_changes", "nssss"},
	    {"ducklake_schema", "nunnssbn"},
	    {"ducklake_table", "nunnnssb"},
	    {"ducklake_view", "nunnnssss"},
	    {"ducklake_tag", "nnnss"},
	    {"ducklake_column_tag", "nnnnss"},
	    // 1.1: tags on a view's columns, keyed on the column NAME rather than an id
	    {"ducklake_view_column_tag", "nsnnss"},
	    {"ducklake_data_file", "nnnnnsbsnnnnnsnnn"},
	    {"ducklake_file_column_stats", "nnnnnnssbsbb"},
	    {"ducklake_file_variant_stats", "nnnssnnnssbs"},
	    {"ducklake_delete_file", "nnnnnsbsnnnsnn"},
	    {"ducklake_column", "nnnnnssssbnss"},
	    {"ducklake_table_stats", "nnnn"},
	    {"ducklake_table_column_stats", "nnbbsssbb"},
	    {"ducklake_partition_info", "nnnn"},
	    {"ducklake_partition_column", "nnnns"},
	    {"ducklake_file_partition_value", "nnns"},
	    {"ducklake_files_scheduled_for_deletion", "nsbt"},
	    {"ducklake_inlined_data_tables", "nsn"},
	    {"ducklake_column_mapping", "nns"},
	    {"ducklake_name_mapping", "nnsnnb"},
	    {"ducklake_schema_versions", "nnn"},
	    {"ducklake_macro", "nnsnn"},
	    {"ducklake_macro_impl", "nnsss"},
	    {"ducklake_macro_parameters", "nnnssss"},
	    {"ducklake_sort_info", "nnnn"},
	    {"ducklake_sort_expression", "nnnssss"},
	};
	return columns;
}

constexpr const char *CATALOG_PREFIX = "{METADATA_CATALOG}.";
constexpr const char *INLINED_DATA_PREFIX = "ducklake_inlined_data_";
constexpr const char *INLINED_DELETE_PREFIX = "ducklake_inlined_delete_";

//! The one catalog column named by a T-SQL reserved word: `key`, in the metadata and tag tables.
//! DuckDB takes it bare; SQL Server takes it bracketed, as the shaping's primary keys already do.
string QuotedIfReserved(const string &word) {
	return StringUtil::Lower(word) == "key" ? "[key]" : word;
}

//! A comma-separated column list with its reserved names bracketed.
string QuotedColumnList(const string &columns) {
	string out;
	for (auto &column : StringUtil::Split(columns, ',')) {
		auto name = column;
		StringUtil::Trim(name);
		out += (out.empty() ? "" : ", ") + QuotedIfReserved(name);
	}
	return out;
}

bool IsIdentifierChar(char c) {
	return StringUtil::CharacterIsAlphaNumeric(c) || c == '_';
}

void SkipSpace(const string &s, idx_t &pos) {
	while (pos < s.size() && StringUtil::CharacterIsSpace(s[pos])) {
		pos++;
	}
}

//! Reads an identifier at pos; empty when there is none.
string ReadIdentifier(const string &s, idx_t &pos) {
	auto start = pos;
	while (pos < s.size() && IsIdentifierChar(s[pos])) {
		pos++;
	}
	return s.substr(start, pos - start);
}

bool ReadLiteral(const string &s, idx_t &pos, Literal &out) {
	SkipSpace(s, pos);
	if (pos >= s.size()) {
		return false;
	}
	auto c = s[pos];
	if (c == '\'') {
		// a string: '' inside is an escaped quote and stays as it is
		auto start = ++pos;
		while (pos < s.size()) {
			if (s[pos] == '\'') {
				if (pos + 1 < s.size() && s[pos + 1] == '\'') {
					pos += 2;
					continue;
				}
				break;
			}
			pos++;
		}
		if (pos >= s.size()) {
			return false;
		}
		out.kind = LiteralKind::STRING;
		out.text = s.substr(start, pos - start);
		pos++;
		return true;
	}
	if (c == '-' || StringUtil::CharacterIsDigit(c)) {
		auto start = pos;
		if (c == '-') {
			pos++;
		}
		bool digits = false;
		while (pos < s.size() && StringUtil::CharacterIsDigit(s[pos])) {
			pos++;
			digits = true;
		}
		if (pos < s.size() && s[pos] == '.') {
			pos++;
			while (pos < s.size() && StringUtil::CharacterIsDigit(s[pos])) {
				pos++;
			}
		}
		if (!digits) {
			return false;
		}
		out.kind = LiteralKind::NUMBER;
		out.text = s.substr(start, pos - start);
		return true;
	}
	auto word = ReadIdentifier(s, pos);
	if (word.empty()) {
		return false;
	}
	auto upper = StringUtil::Upper(word);
	if (upper == "NULL") {
		out.kind = LiteralKind::NULL_VALUE;
		return true;
	}
	if (upper == "TRUE" || upper == "FALSE") {
		out.kind = LiteralKind::BOOLEAN;
		out.text = upper == "TRUE" ? "1" : "0";
		return true;
	}
	if (upper == "NOW" && pos + 1 < s.size() && s[pos] == '(' && s[pos + 1] == ')') {
		pos += 2;
		out.kind = LiteralKind::NOW;
		return true;
	}
	return false;
}

//! `(lit, lit, ...)[, (lit, ...)]*` up to the end of the text. False on anything else.
bool ReadTuples(const string &s, idx_t &pos, vector<vector<Literal>> &tuples) {
	while (true) {
		SkipSpace(s, pos);
		if (pos >= s.size() || s[pos] != '(') {
			return false;
		}
		pos++;
		vector<Literal> tuple;
		while (true) {
			Literal lit;
			if (!ReadLiteral(s, pos, lit)) {
				return false;
			}
			tuple.push_back(std::move(lit));
			SkipSpace(s, pos);
			if (pos >= s.size()) {
				return false;
			}
			if (s[pos] == ',') {
				pos++;
				continue;
			}
			if (s[pos] == ')') {
				pos++;
				break;
			}
			return false;
		}
		tuples.push_back(std::move(tuple));
		SkipSpace(s, pos);
		if (pos >= s.size()) {
			return true;
		}
		if (s[pos] != ',') {
			return false;
		}
		pos++;
	}
}

//! A literal rendered for a column of the given kind. False when they do not go together.
bool RenderLiteral(const Literal &lit, char column_kind, string &out) {
	switch (lit.kind) {
	case LiteralKind::NULL_VALUE:
		out = "NULL";
		return true;
	case LiteralKind::NUMBER:
		if (column_kind != 'n') {
			return false;
		}
		out = lit.text;
		return true;
	case LiteralKind::STRING:
		// N'...': the text travels as UTF-16 and lands in the UTF-8 column without passing through
		// the database's own code page, which is usually a legacy one
		if (column_kind != 's' && column_kind != 'u') {
			return false;
		}
		out = "N'" + lit.text + "'";
		return true;
	case LiteralKind::BOOLEAN:
		if (column_kind != 'b') {
			return false;
		}
		out = lit.text;
		return true;
	case LiteralKind::NOW:
		if (column_kind != 't') {
			return false;
		}
		out = "SYSDATETIMEOFFSET()";
		return true;
	}
	return false;
}

//! A literal rendered on its own kind - inside a CTE's VALUES, where the column is the literal's.
string RenderLiteral(const Literal &lit) {
	switch (lit.kind) {
	case LiteralKind::NULL_VALUE:
		return "NULL";
	case LiteralKind::STRING:
		return "N'" + lit.text + "'";
	case LiteralKind::NOW:
		return "SYSDATETIMEOFFSET()";
	default:
		return lit.text;
	}
}

//! `head` + up to a thousand comma-separated rows + `tail`, repeated until the rows are out - the
//! same statement, once per chunk, in one batch.
string ChunkedStatements(const string &head, const vector<string> &rows, const string &tail) {
	static constexpr idx_t ROWS_PER_STATEMENT = 1000;
	string out;
	for (idx_t start = 0; start < rows.size(); start += ROWS_PER_STATEMENT) {
		auto end = MinValue<idx_t>(start + ROWS_PER_STATEMENT, rows.size());
		string values;
		for (idx_t i = start; i < end; i++) {
			values += (i == start ? "" : ", ") + rows[i];
		}
		out += (out.empty() ? "" : "\n") + head + values + tail;
	}
	return out;
}

//! `INSERT INTO {METADATA_CATALOG}.<table> VALUES (...)[, (...)]` for a table on the list. The
//! macros' generator writes `values(` in lower case with no space; both forms are the same statement.
const unordered_map<string, string> &CatalogColumnsV1_0() {
	static const unordered_map<string, string> columns = [] {
		auto result = CatalogColumnsV1_1();
		const vector<pair<string, idx_t>> added_by_v1_1 = {
		    {"ducklake_data_file", 1},          {"ducklake_delete_file", 1}, {"ducklake_file_column_stats", 2},
		    {"ducklake_table_column_stats", 2}, {"ducklake_schema", 1},
		};
		for (auto &entry : added_by_v1_1) {
			auto &kinds = result.at(entry.first);
			kinds.resize(kinds.size() - entry.second);
		}
		result.erase("ducklake_view_column_tag");
		return result;
	}();
	return columns;
}

const unordered_map<string, string> &CatalogColumns(bool v1_1) {
	return v1_1 ? CatalogColumnsV1_1() : CatalogColumnsV1_0();
}

//! The statistics columns of the tables that carry them, in DDL order (format 1.1): a bound longer
//! than the catalog's stats_length is written as NULL, and so is its exactness (specs/018).
const unordered_map<string, vector<string>> &StatsColumnOrder() {
	static const unordered_map<string, vector<string>> order = {
	    {"ducklake_file_column_stats",
	     {"data_file_id", "table_id", "column_id", "column_size_bytes", "value_count", "null_count", "min_value",
	      "max_value", "contains_nan", "extra_stats", "min_is_exact", "max_is_exact"}},
	    {"ducklake_table_column_stats",
	     {"table_id", "column_id", "contains_null", "contains_nan", "min_value", "max_value", "extra_stats",
	      "min_is_exact", "max_is_exact"}},
	    {"ducklake_file_variant_stats",
	     {"data_file_id", "table_id", "column_id", "variant_path", "shredded_type", "column_size_bytes", "value_count",
	      "null_count", "min_value", "max_value", "contains_nan", "extra_stats"}},
	};
	return order;
}

//! The bytes a string literal stands for: its text with each doubled quote counted once.
idx_t LiteralBytes(const Literal &lit) {
	idx_t doubled = 0;
	for (idx_t i = 0; i + 1 < lit.text.size(); i++) {
		if (lit.text[i] == '\'' && lit.text[i + 1] == '\'') {
			doubled++;
			i++;
		}
	}
	return lit.text.size() - doubled;
}

//! A min or max past the bound becomes NULL - unknown, which DuckLake reads as "do not prune on it" and
//! never resurrects in a merge (ducklake_stats.cpp) - and its exactness with it.
void BoundStats(const string &table, const vector<string> &listed, int64_t stats_length,
                vector<vector<Literal>> &tuples) {
	if (stats_length <= 0) {
		return;
	}
	auto entry = StatsColumnOrder().find(table);
	if (entry == StatsColumnOrder().end()) {
		return;
	}
	auto &columns = listed.empty() ? entry->second : listed;
	auto position = [&](const char *name) -> optional_idx {
		for (idx_t i = 0; i < columns.size(); i++) {
			if (columns[i] == name) {
				return i;
			}
		}
		return optional_idx();
	};
	const pair<const char *, const char *> bounds[] = {{"min_value", "min_is_exact"}, {"max_value", "max_is_exact"}};
	for (auto &tuple : tuples) {
		for (auto &bound : bounds) {
			auto value = position(bound.first);
			if (!value.IsValid() || value.GetIndex() >= tuple.size()) {
				continue;
			}
			auto &lit = tuple[value.GetIndex()];
			if (lit.kind != LiteralKind::STRING || LiteralBytes(lit) <= idx_t(stats_length)) {
				continue;
			}
			lit = Literal {LiteralKind::NULL_VALUE, string()};
			auto exact = position(bound.second);
			if (exact.IsValid() && exact.GetIndex() < tuple.size()) {
				tuple[exact.GetIndex()] = Literal {LiteralKind::NULL_VALUE, string()};
			}
		}
	}
}

bool RewriteInsert(const string &stmt, const string &schema, bool v1_1, int64_t stats_length, string &tsql) {
	const string head = string("INSERT INTO ") + CATALOG_PREFIX;
	if (!StringUtil::StartsWith(stmt, head)) {
		return false;
	}
	idx_t pos = head.size();
	auto table = ReadIdentifier(stmt, pos);
	string kinds;
	if (StringUtil::StartsWith(table, INLINED_DELETE_PREFIX)) {
		kinds = "nnn";
	} else {
		auto &columns = CatalogColumns(v1_1);
		auto entry = columns.find(table);
		if (entry == columns.end()) {
			return false;
		}
		kinds = entry->second;
	}
	SkipSpace(stmt, pos);
	// 1.1 writes some of these with an explicit column list -
	// `INSERT INTO t (schema_id, schema_uuid, …) VALUES (…)`. T-SQL takes the same list, so it is
	// carried through verbatim; the kinds stay positional, and a listed column count that does not
	// match the table's refuses the rewrite rather than guessing (the strict guard then names it).
	string column_list;
	vector<string> listed_columns;
	if (pos < stmt.size() && stmt[pos] == '(') {
		auto close = stmt.find(')', pos);
		if (close == string::npos) {
			return false;
		}
		auto listed = stmt.substr(pos + 1, close - pos - 1);
		idx_t listed_count = 1;
		for (auto ch : listed) {
			if (ch == ',') {
				listed_count++;
			}
		}
		if (listed_count != kinds.size()) {
			return false;
		}
		string rendered_list;
		for (auto &name : StringUtil::Split(listed, ',')) {
			auto trimmed = name;
			StringUtil::Trim(trimmed);
			for (auto ch : trimmed) {
				if (!IsIdentifierChar(ch)) {
					return false;
				}
			}
			rendered_list += (rendered_list.empty() ? "" : ", ") + QuotedIfReserved(trimmed);
			listed_columns.push_back(trimmed);
		}
		column_list = " (" + rendered_list + ")";
		pos = close + 1;
		SkipSpace(stmt, pos);
	}
	auto keyword = ReadIdentifier(stmt, pos);
	if (StringUtil::Upper(keyword) != "VALUES") {
		return false;
	}
	vector<vector<Literal>> tuples;
	if (!ReadTuples(stmt, pos, tuples) || tuples.empty()) {
		return false;
	}
	BoundStats(table, listed_columns, stats_length, tuples);
	// SQL Server takes at most 1000 rows in one VALUES list (error 10738); a commit of two hundred
	// files writes eight thousand statistics rows in one INSERT, so the statement is emitted per
	// thousand rows - still one round trip, since the run is one batch
	vector<string> rows;
	for (auto &tuple : tuples) {
		if (tuple.size() != kinds.size()) {
			return false;
		}
		string rendered;
		for (idx_t i = 0; i < tuple.size(); i++) {
			string cell;
			if (!RenderLiteral(tuple[i], kinds[i], cell)) {
				return false;
			}
			rendered += (i == 0 ? "" : ", ") + cell;
		}
		rows.push_back("(" + rendered + ")");
	}
	tsql = ChunkedStatements("INSERT INTO " + schema + "." + table + column_list + " VALUES ", rows, ";");
	return true;
}

//! The predicate and assignment text of an UPDATE or DELETE, checked against what those families
//! carry: names, numbers, punctuation, keywords - no strings, and no call other than IN (...) and
//! EXISTS (...). What passes is T-SQL already. `{METADATA_CATALOG}.` inside (a NOT EXISTS subquery)
//! is swapped for the schema.
bool NumericBody(const string &body, const string &schema, string &out) {
	string result;
	idx_t pos = 0;
	while (pos < body.size()) {
		auto c = body[pos];
		if (StringUtil::CharacterIsSpace(c) || c == '(' || c == ')' || c == ',' || c == '=' || c == '<' || c == '>' ||
		    c == '!' || c == '.') {
			result += c;
			pos++;
			continue;
		}
		if (c == '{') {
			if (body.compare(pos, strlen(CATALOG_PREFIX), CATALOG_PREFIX) != 0) {
				return false;
			}
			result += schema + ".";
			pos += strlen(CATALOG_PREFIX);
			continue;
		}
		if (StringUtil::CharacterIsDigit(c) || c == '-') {
			Literal lit;
			if (!ReadLiteral(body, pos, lit) || lit.kind != LiteralKind::NUMBER) {
				return false;
			}
			result += lit.text;
			continue;
		}
		if (c == '\'') {
			// 1.1 writes the per-column statistics refresh as a direct UPDATE with the min/max
			// values as string literals in the SET list, where 1.0 carried them in a CTE's VALUES.
			// The literal is rendered the way every other catalog string is: N'...', so the text
			// travels as UTF-16 into the UTF-8 column rather than through the database's code page.
			Literal lit;
			if (!ReadLiteral(body, pos, lit) || lit.kind != LiteralKind::STRING) {
				return false;
			}
			result += "N'" + lit.text + "'";
			continue;
		}
		if (IsIdentifierChar(c)) {
			auto word = ReadIdentifier(body, pos);
			// 1.1 writes `CAST(true AS BOOLEAN)` into the stats refresh's SET list, and DuckDB's
			// spelling of a boolean is not T-SQL's: `true`/`false` are column references to SQL
			// Server (error 207 "Invalid column name 'true'"), and BOOLEAN is not a type name
			auto upper_word = StringUtil::Upper(word);
			if (upper_word == "TRUE" || upper_word == "FALSE") {
				result += upper_word == "TRUE" ? "1" : "0";
				continue;
			}
			if (upper_word == "BOOLEAN") {
				result += "BIT";
				continue;
			}
			auto next = pos;
			SkipSpace(body, next);
			if (next < body.size() && body[next] == '(') {
				// the calls these families carry: IN (...), NOT EXISTS (...), and the CTE updates'
				// CAST(x AS BIT), already rewritten from BOOLEAN by the caller - and a parenthesised
				// condition after a connective, as the flush's delete of its inlined rows writes since
				// ducklake main: `... <= N AND (_ducklake_end_snapshot IS NULL OR ... <= N)`
				auto upper = StringUtil::Upper(word);
				if (upper != "IN" && upper != "EXISTS" && upper != "CAST" && upper != "AND" && upper != "OR" &&
				    upper != "NOT") {
					return false;
				}
			}
			result += QuotedIfReserved(word);
			continue;
		}
		return false;
	}
	out = result;
	return true;
}

//! `UPDATE {METADATA_CATALOG}.<t> SET ... WHERE ...` with a numeric body.
bool RewriteUpdate(const string &stmt, const string &schema, string &tsql) {
	const string head = string("UPDATE ") + CATALOG_PREFIX;
	if (!StringUtil::StartsWith(stmt, head)) {
		return false;
	}
	idx_t pos = head.size();
	auto table = ReadIdentifier(stmt, pos);
	if (table.empty()) {
		return false;
	}
	string body;
	if (!NumericBody(stmt.substr(pos), schema, body)) {
		return false;
	}
	tsql = "UPDATE " + schema + "." + table + body + ";";
	return true;
}

//! `DELETE FROM {METADATA_CATALOG}.<t> [alias] [WHERE ...]` with a numeric body. SQL Server refuses
//! an alias after the target in this form; the alias form becomes `DELETE alias FROM t alias ...`.
bool RewriteDelete(const string &stmt, const string &schema, string &tsql) {
	const string head = string("DELETE FROM ") + CATALOG_PREFIX;
	if (!StringUtil::StartsWith(stmt, head)) {
		return false;
	}
	idx_t pos = head.size();
	auto table = ReadIdentifier(stmt, pos);
	if (table.empty()) {
		return false;
	}
	auto after = pos;
	SkipSpace(stmt, after);
	auto word = ReadIdentifier(stmt, after);
	string alias;
	if (!word.empty() && StringUtil::Upper(word) != "WHERE") {
		alias = word;
		pos = after;
	}
	string body;
	if (!NumericBody(stmt.substr(pos), schema, body)) {
		return false;
	}
	if (alias.empty()) {
		tsql = "DELETE FROM " + schema + "." + table + body + ";";
	} else {
		tsql = "DELETE " + alias + " FROM " + schema + "." + table + " " + alias + body + ";";
	}
	return true;
}

//! `WITH <cte>(cols) AS (VALUES ...) UPDATE {METADATA_CATALOG}.<t> SET ... FROM <cte> WHERE ...` - the
//! stats refresh, inlined-row deletes, dropped columns, overwritten tags. T-SQL wants the VALUES
//! behind a SELECT, BIT for BOOLEAN, and accepts the target outside FROM (verified, specs/014 D2).
bool RewriteCteUpdate(const string &stmt, const string &schema, string &tsql) {
	if (!StringUtil::StartsWith(stmt, "WITH ")) {
		return false;
	}
	idx_t pos = 5;
	auto cte = ReadIdentifier(stmt, pos);
	if (cte.empty() || pos >= stmt.size() || stmt[pos] != '(') {
		return false;
	}
	auto columns_end = stmt.find(')', pos);
	if (columns_end == string::npos) {
		return false;
	}
	auto columns = stmt.substr(pos + 1, columns_end - pos - 1);
	pos = columns_end + 1;
	SkipSpace(stmt, pos);
	if (stmt.compare(pos, 3, "AS ") != 0 && stmt.compare(pos, 3, "AS\n") != 0) {
		return false;
	}
	pos += 2;
	SkipSpace(stmt, pos);
	if (pos >= stmt.size() || stmt[pos] != '(') {
		return false;
	}
	pos++;
	SkipSpace(stmt, pos);
	if (stmt.compare(pos, 6, "VALUES") != 0) {
		return false;
	}
	pos += 6;
	// the tuples run to the parenthesis that closes the CTE body: find it by depth, then parse
	auto tuples_start = pos;
	idx_t depth = 1;
	bool quote = false;
	idx_t body_end = string::npos;
	for (idx_t i = pos; i < stmt.size(); i++) {
		auto c = stmt[i];
		if (c == '\'') {
			quote = !quote;
			continue;
		}
		if (quote) {
			continue;
		}
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			depth--;
			if (depth == 0) {
				body_end = i;
				break;
			}
		}
	}
	if (body_end == string::npos) {
		return false;
	}
	auto tuples_text = stmt.substr(tuples_start, body_end - tuples_start);
	idx_t tpos = 0;
	vector<vector<Literal>> tuples;
	if (!ReadTuples(tuples_text, tpos, tuples) || tuples.empty()) {
		return false;
	}
	vector<string> rows;
	for (auto &tuple : tuples) {
		string rendered;
		for (idx_t i = 0; i < tuple.size(); i++) {
			rendered += (i == 0 ? "" : ", ") + RenderLiteral(tuple[i]);
		}
		rows.push_back("(" + rendered + ")");
	}
	pos = body_end + 1;
	SkipSpace(stmt, pos);
	const string head = string("UPDATE ") + CATALOG_PREFIX;
	if (stmt.compare(pos, head.size(), head) != 0) {
		return false;
	}
	pos += head.size();
	auto table = ReadIdentifier(stmt, pos);
	if (table.empty()) {
		return false;
	}
	// the assignments and predicates: numeric, names, and CAST(x AS BOOLEAN), which becomes BIT
	string body;
	if (!NumericBody(StringUtil::Replace(stmt.substr(pos), " AS BOOLEAN)", " AS BIT)"), schema, body)) {
		return false;
	}
	// the rows of a CTE's VALUES are independent updates, so the statement repeats per thousand
	const auto quoted_columns = QuotedColumnList(columns);
	tsql = ChunkedStatements("WITH " + cte + "(" + quoted_columns + ") AS (SELECT * FROM (VALUES ", rows,
	                         ") AS v(" + quoted_columns + ")) UPDATE " + schema + "." + table + body + ";");
	return true;
}

bool RewriteDropIfExists(const string &stmt, const string &schema, string &tsql, string &dropped) {
	const string head = string("DROP TABLE IF EXISTS ") + CATALOG_PREFIX;
	if (!StringUtil::StartsWith(stmt, head)) {
		return false;
	}
	idx_t pos = head.size();
	auto table = ReadIdentifier(stmt, pos);
	SkipSpace(stmt, pos);
	if (table.empty() || pos != stmt.size()) {
		return false;
	}
	dropped = table;
	tsql = "DROP TABLE IF EXISTS " + schema + "." + table + ";";
	return true;
}

//! The batch split on `;` outside quotes, each statement trimmed; empty ones dropped.
vector<string> SplitStatements(const string &batch) {
	vector<string> out;
	string current;
	bool single = false, dbl = false;
	for (auto c : batch) {
		if (c == '\'' && !dbl) {
			single = !single;
		} else if (c == '"' && !single) {
			dbl = !dbl;
		}
		if (c == ';' && !single && !dbl) {
			StringUtil::Trim(current);
			if (!current.empty()) {
				out.push_back(current);
			}
			current.clear();
			continue;
		}
		current += c;
	}
	StringUtil::Trim(current);
	if (!current.empty()) {
		out.push_back(current);
	}
	return out;
}

//! One per-column refresh of `ducklake_table_column_stats`, as UpdateGlobalTableStatsSql writes it
//! since ducklake's v1.5 head: `UPDATE {METADATA_CATALOG}.ducklake_table_column_stats SET
//! contains_null=CAST(<b> AS BOOLEAN), contains_nan=CAST(<b> AS BOOLEAN), min_value=<s>,
//! max_value=<s>, extra_stats=<s>[, min_is_exact=CAST(<b> AS BOOLEAN), max_is_exact=CAST(<b> AS
//! BOOLEAN)] WHERE table_id=<n> AND column_id=<n>`, values already T-SQL.
struct ColumnStatsRefresh {
	string table_id;
	string column_id;
	bool exactness = false;
	//! contains_null, contains_nan, min, max, extra[, min_is_exact, max_is_exact]
	vector<string> values;
};

bool ReadColumnStatsRefresh(const string &stmt, ColumnStatsRefresh &out) {
	idx_t pos = 0;
	auto expect = [&](const char *text) {
		auto n = strlen(text);
		if (stmt.compare(pos, n, text) != 0) {
			return false;
		}
		pos += n;
		return true;
	};
	auto boolean = [&](string &value) {
		Literal lit;
		if (!expect("CAST(") || !ReadLiteral(stmt, pos, lit) || !expect(" AS BOOLEAN)")) {
			return false;
		}
		// a bare 1/0/NULL: the BIT column converts it on assignment, and the batch is a third shorter
		// than with a CAST per value - its text is parsed on every commit
		if (lit.kind == LiteralKind::BOOLEAN) {
			value = lit.text;
		} else if (lit.kind == LiteralKind::NULL_VALUE) {
			value = "NULL";
		} else {
			return false;
		}
		return true;
	};
	auto text = [&](string &value) {
		Literal lit;
		if (!ReadLiteral(stmt, pos, lit)) {
			return false;
		}
		if (lit.kind == LiteralKind::STRING) {
			// N'...': UTF-16 into the UTF-8 column, not through the database's code page
			value = "N'" + lit.text + "'";
		} else if (lit.kind == LiteralKind::NULL_VALUE) {
			value = "NULL";
		} else {
			return false;
		}
		return true;
	};
	auto number = [&](string &value) {
		Literal lit;
		if (!ReadLiteral(stmt, pos, lit) || lit.kind != LiteralKind::NUMBER) {
			return false;
		}
		value = lit.text;
		return true;
	};
	out.values.assign(5, string());
	if (!expect("UPDATE ") || !expect(CATALOG_PREFIX) || !expect("ducklake_table_column_stats SET contains_null=") ||
	    !boolean(out.values[0]) || !expect(", contains_nan=") || !boolean(out.values[1]) || !expect(", min_value=") ||
	    !text(out.values[2]) || !expect(", max_value=") || !text(out.values[3]) || !expect(", extra_stats=") ||
	    !text(out.values[4])) {
		return false;
	}
	out.exactness = stmt.compare(pos, 15, ", min_is_exact=") == 0;
	if (out.exactness) {
		out.values.resize(7);
		if (!expect(", min_is_exact=") || !boolean(out.values[5]) || !expect(", max_is_exact=") ||
		    !boolean(out.values[6])) {
			return false;
		}
	}
	return expect(" WHERE table_id=") && number(out.table_id) && expect(" AND column_id=") && number(out.column_id) &&
	       pos == stmt.size();
}

//! The refresh's min and max past the bound become NULL, their exactness too (specs/018).
void BoundRefresh(ColumnStatsRefresh &refresh, int64_t stats_length) {
	if (stats_length <= 0) {
		return;
	}
	for (idx_t bound = 2; bound <= 3; bound++) {
		auto &value = refresh.values[bound];
		if (!StringUtil::StartsWith(value, "N'")) {
			continue;
		}
		Literal lit {LiteralKind::STRING, value.substr(2, value.size() - 3)};
		if (LiteralBytes(lit) <= idx_t(stats_length)) {
			continue;
		}
		value = "NULL";
		if (refresh.exactness) {
			refresh.values[bound + 3] = "NULL";
		}
	}
}

//! A run of those for one table as ONE statement: the 1.0 shape, `UPDATE ... FROM (VALUES ...)`.
//! DuckLake split it into a statement per column for a DuckDB-backed catalog, whose multi-row VALUES
//! corrupted long strings beside NULLs; SQL Server has no such bug, and 41 statements cost ~4 ms of
//! a commit's ~23 against one (specs/015).
string CoalescedColumnStatsRefresh(const string &schema, const vector<ColumnStatsRefresh> &rows) {
	static constexpr idx_t ROWS_PER_STATEMENT = 1000;
	auto exactness = rows[0].exactness;
	string set = "contains_null = v.contains_null, contains_nan = v.contains_nan, min_value = v.min_value, "
	             "max_value = v.max_value, extra_stats = v.extra_stats";
	string names = "column_id, contains_null, contains_nan, min_value, max_value, extra_stats";
	if (exactness) {
		set += ", min_is_exact = v.min_is_exact, max_is_exact = v.max_is_exact";
		names += ", min_is_exact, max_is_exact";
	}
	string out;
	for (idx_t start = 0; start < rows.size(); start += ROWS_PER_STATEMENT) {
		auto end = MinValue<idx_t>(start + ROWS_PER_STATEMENT, rows.size());
		string values;
		for (idx_t i = start; i < end; i++) {
			values += (i == start ? "(" : ", (") + rows[i].column_id;
			for (auto &value : rows[i].values) {
				values += ", " + value;
			}
			values += ")";
		}
		out += StringUtil::Format("UPDATE s SET %s FROM %s.ducklake_table_column_stats s JOIN (VALUES %s) v(%s) "
		                          "ON s.table_id = %s AND s.column_id = v.column_id;\n",
		                          set, schema, values, names, rows[0].table_id);
	}
	return out;
}

bool IsInlinedRowsInsert(const string &stmt) {
	const string head = string("INSERT INTO ") + CATALOG_PREFIX + INLINED_DATA_PREFIX;
	return StringUtil::StartsWith(stmt, head) && !StringUtil::StartsWith(stmt, head + "tables");
}

} // namespace

//! The DDL DuckLake's commit loop writes into the batch for a new inlined deletion table, verbatim
//! from WriteNewInlinedFileDeletesSqlBatch - matched exactly, the way the conflict check is
//! (specs/007), and guarded at attach the same way. Between the two halves sits the table id.
constexpr const char *INLINED_DELETE_DDL_HEAD =
    "CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.ducklake_inlined_delete_";
constexpr const char *INLINED_DELETE_DDL_TAIL = "(file_id BIGINT, row_id BIGINT, begin_snapshot BIGINT)";

bool InlinedDeletionDdlIsDuckLakes(DuckLakeMetadataManager &manager) {
	DuckLakeInlinedFileDeletionInfo probe;
	probe.table_id = TableIndex(7);
	vector<DuckLakeInlinedFileDeletionInfo> one;
	one.push_back(std::move(probe));
	// ducklake main made this a member (it marks the manager's cache for clearing); on the v1.5 line
	// it was static. RECON: the real port wants a check that does not touch the live manager.
	auto generated = manager.WriteNewInlinedFileDeletesSqlBatch(one);
	return StringUtil::StartsWith(generated, string(INLINED_DELETE_DDL_HEAD) + "7" + INLINED_DELETE_DDL_TAIL + ";");
}

unique_ptr<QueryResult> MSSQLMetadataManager::RunCommitBatch(const string &tsql) {
	auto &connection = transaction.GetConnection();
	return TracedQuery(connection, StringUtil::Format("SELECT mssql_exec(%s, %s)", CatalogLiteral(), SQLString(tsql)));
}

//===--------------------------------------------------------------------===//
// The user's inlined rows in the run (specs/015)
//===--------------------------------------------------------------------===//

namespace {

//! T-SQL's limit on the rows of one VALUES
constexpr idx_t MAX_STATEMENT_ROWS = 1000;

//! One value of an inlined row as a T-SQL literal for its column (TSQLColumnType), or false when it
//! is not ours to render - the row set then goes to the base, DuckDB's DML path. A type the column
//! holds as text is the text DuckDB's own cast gives, the cast an INSERT through the attached
//! catalog does (an interval excepted, written in DuckLake's own form); strings are N literals, so they reach the UTF-8
//! column without the database's code page. The literals are what a FORCED-parameterized batch turns into parameters,
//! so the run's plan is reused across commits (specs/012) - measured against mssql_exec_params, which puts the whole
//! run inside sp_executesql, where forced parameterization does not apply and every commit compiled its run: 14 ms a
//! commit against 4.
bool RenderInlinedValue(DuckLakeMetadataManager &manager, ClientContext &context, const Value &value, string &out) {
	if (value.IsNull()) {
		out = "NULL";
		return true;
	}
	auto &type = value.type();
	if (type.HasAlias() || type.IsNested() || DuckLakeUtil::GetInlinedStorageType(manager, type) != type) {
		return false;
	}
	auto n_literal = [&](const string &text) {
		if (text.find('\0') != string::npos) {
			return false;
		}
		out = "N'" + StringUtil::Replace(text, "'", "''") + "'";
		return true;
	};
	if (!manager.TypeIsNativelySupported(type)) {
		switch (type.id()) {
		case LogicalTypeId::VARIANT:
		case LogicalTypeId::GEOMETRY:
		case LogicalTypeId::BIT:
		case LogicalTypeId::ENUM:
			return false;
		case LogicalTypeId::INTERVAL: {
			// DuckLake's canonical text for an interval, not DuckDB's (ToSQLString in ducklake_util)
			auto interval = IntervalValue::Get(value);
			return n_literal(StringUtil::Format("%d months %d days %lld microseconds", interval.months, interval.days,
			                                    interval.micros));
		}
		default:
			return n_literal(value.CastAs(context, LogicalType::VARCHAR).GetValue<string>());
		}
	}
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		out = value.GetValue<bool>() ? "1" : "0";
		return true;
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::DECIMAL:
		out = value.ToString();
		return true;
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::UUID:
		return n_literal(value.ToString());
	case LogicalTypeId::BLOB: {
		auto &bytes = StringValue::Get(value);
		static constexpr const char *HEX = "0123456789ABCDEF";
		out = "0x";
		out.reserve(2 + bytes.size() * 2);
		for (auto c : bytes) {
			auto byte = static_cast<uint8_t>(c);
			out += HEX[byte >> 4];
			out += HEX[byte & 0x0F];
		}
		return true;
	}
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC: {
		// ISO text, which date and datetime2 read whatever the session's language; a year past four
		// digits, a BC date and infinity are not theirs
		auto text = value.ToString();
		if (!HasFourDigitYear(text) || text.find('(') != string::npos) {
			return false;
		}
		return n_literal(text);
	}
	case LogicalTypeId::TIMESTAMP_TZ: {
		// the instant in UTC, whatever the session's time zone, as DuckDB prints it without one
		auto text = Timestamp::ToString(value.GetValue<timestamp_t>());
		if (!HasFourDigitYear(text) || text.find('(') != string::npos) {
			return false;
		}
		return n_literal(text + "+00:00");
	}
	case LogicalTypeId::TIME: {
		auto text = value.ToString();
		if (StringUtil::StartsWith(text, "24")) {
			return false;
		}
		return n_literal(text);
	}
	default:
		return false;
	}
}

} // namespace

string MSSQLMetadataManager::WriteNewInlinedData(DuckLakeSnapshot &commit_snapshot,
                                                 const vector<DuckLakeInlinedDataInfo> &new_data,
                                                 const vector<DuckLakeTableInfo> &new_tables,
                                                 const vector<DuckLakeTableInfo> &new_inlined_data_tables_result,
                                                 vector<unique_ptr<SQLStatement>> &inlined_inserts) {
	// a batch is built afresh on every attempt of the commit loop, and only the last one runs
	inlined_rows_statements.clear();
	if (InlinedRowsInRunDisabled() || !BatchRewriteEnabled()) {
		return DuckLakeMetadataManager::WriteNewInlinedData(commit_snapshot, new_data, new_tables,
		                                                    new_inlined_data_tables_result, inlined_inserts);
	}
	auto context = transaction.context.lock();
	string batch;
	for (auto &entry : new_data) {
		// The rows first: rendered whole, or the row set is the base's
		vector<pair<int64_t, string>> rows;
		bool rendered = entry.data && entry.data->data;
		if (rendered) {
			auto &data = *entry.data;
			const bool preserved = data.HasPreservedRowIds();
			idx_t next_row_id = entry.row_id_start;
			idx_t position = 0;
			for (auto &chunk : data.data->Chunks()) {
				for (idx_t r = 0; rendered && r < chunk.size(); r++, position++) {
					int64_t row_id;
					if (preserved && !DuckLakeConstants::IsTransactionLocalRowId(data.row_ids[position])) {
						row_id = data.row_ids[position];
					} else {
						row_id = NumericCast<int64_t>(next_row_id++);
					}
					string values, literal;
					for (idx_t c = 0; rendered && c < chunk.ColumnCount(); c++) {
						rendered = RenderInlinedValue(*this, *context, chunk.GetValue(c, r), literal);
						values += ", " + literal;
					}
					rows.emplace_back(row_id, std::move(values));
				}
				if (!rendered) {
					break;
				}
			}
		}
		if (!rendered) {
			// the base's own statement for this row set, which takes DuckDB's DML path in Execute
			vector<DuckLakeInlinedDataInfo> one {entry};
			batch += DuckLakeMetadataManager::WriteNewInlinedData(commit_snapshot, one, new_tables,
			                                                      new_inlined_data_tables_result, inlined_inserts);
			continue;
		}

		// The inlined table: the latest one of the lake table, or a new one - the base's
		// WriteNewInlinedData step for step (re-audit at a ducklake bump), its name cache ours.
		optional_ptr<const DuckLakeTableInfo> new_inlined_table;
		for (auto &inlined_table : new_inlined_data_tables_result) {
			if (inlined_table.id == entry.table_id) {
				new_inlined_table = &inlined_table;
				break;
			}
		}
		string inlined_table_name;
		auto known = inlined_table_names.find(entry.table_id.index);
		if (known != inlined_table_names.end() && known->second.commit_snapshot_id == commit_snapshot.snapshot_id) {
			inlined_table_name = known->second.name;
		}
		if (inlined_table_name.empty() && !new_inlined_table) {
			auto lookup = LatestInlinedTableQuery(entry.table_id.index) + ";";
			auto result = Query(commit_snapshot, lookup);
			for (auto &row : *result) {
				inlined_table_name = row.GetValue<string>(0);
				inlined_table_names[entry.table_id.index] = {commit_snapshot.snapshot_id, inlined_table_name};
			}
		}
		if (inlined_table_name.empty()) {
			DuckLakeTableInfo table_info;
			if (new_inlined_table) {
				table_info = *new_inlined_table;
			} else {
				auto table_entry =
				    transaction.GetCatalog().GetEntryById(transaction, transaction.GetSnapshot(), entry.table_id);
				if (table_entry) {
					auto &table = table_entry->Cast<DuckLakeTableEntry>();
					table_info = table.GetTableInfo();
					table_info.columns = table.GetTableColumns();
				} else {
					bool found = false;
					for (auto &new_table : new_tables) {
						if (new_table.id == entry.table_id) {
							table_info = new_table;
							found = true;
						}
					}
					if (!found) {
						throw InternalException("Writing inlined data for a table that cannot be found in the catalog");
					}
				}
				commit_snapshot.schema_version++;
			}
			vector<string> inlined_tables;
			string inlined_table_queries;
			inlined_table_name =
			    GetInlinedTableQueries(commit_snapshot, table_info, inlined_tables, inlined_table_queries);
			batch += InsertValuesSql("ducklake_inlined_data_tables", inlined_tables);
			batch += inlined_table_queries;
		}

		// The rows' statement, held here; the batch carries its marker
		const auto head =
		    StringUtil::Format("INSERT INTO %s.%s VALUES ", SchemaIdentifier(), SQLIdentifier(inlined_table_name));
		for (idx_t start = 0; start < rows.size(); start += MAX_STATEMENT_ROWS) {
			InlinedRowsStatement statement;
			statement.head = head;
			auto end = MinValue<idx_t>(start + MAX_STATEMENT_ROWS, rows.size());
			for (idx_t i = start; i < end; i++) {
				statement.rows.push_back(std::move(rows[i]));
			}
			batch += INLINED_ROWS_MARKER + to_string(inlined_rows_statements.size()) + ";";
			inlined_rows_statements.push_back(std::move(statement));
		}
	}
	return batch;
}

unique_ptr<QueryResult> MSSQLMetadataManager::TryRewriteWrite(DuckLakeSnapshot snapshot, const string &query) {
	if (!BatchRewriteEnabled()) {
		return nullptr;
	}
	string statement = query;
	SubstituteSnapshotPlaceholders(snapshot, statement);
	return RewriteWriteStatement(std::move(statement));
}

unique_ptr<QueryResult> MSSQLMetadataManager::TryRewriteWrite(const string &query) {
	// Query(string &) substitutes no snapshot, so a statement on this path carries no snapshot
	// placeholder by contract; one that does is not ours to guess at
	if (!BatchRewriteEnabled() || query.find("{SNAPSHOT_ID}") != string::npos ||
	    query.find("{SCHEMA_VERSION}") != string::npos || query.find("{NEXT_") != string::npos) {
		return nullptr;
	}
	return RewriteWriteStatement(query);
}

unique_ptr<QueryResult> MSSQLMetadataManager::RewriteWriteStatement(string query) {
	// Not every write is in the commit batch: the expiry and the cleanup DELETE from a dozen tables
	// through Query, one statement at a time, the flush DELETEs the rows it moved to files, and the
	// drop of superseded inlined tables sends several DELETEs and DROPs in one string. On the base
	// path each DELETE is a scan of the table through the extension's DML operator plus the DELETE
	// by row identity - and on linux_amd64 the composite-key row identity of ducklake_tag came back
	// as invalid unicode, not every time (CI, specs/014 D3c). The statements are the batch's own
	// families, so they take the batch's path: one T-SQL run, one round trip. A query with a
	// statement that is not a write at all is a read and goes to the base whole.
	const auto statements = SplitStatements(query);
	if (statements.empty()) {
		return nullptr;
	}
	const auto schema = SchemaIdentifier();
	const string update_head = string("UPDATE ") + CATALOG_PREFIX;
	const string delete_head = string("DELETE FROM ") + CATALOG_PREFIX;
	const string drop_head = string("DROP TABLE IF EXISTS ") + CATALOG_PREFIX;
	vector<string> pieces;
	vector<string> dropped_in_run;
	for (auto &statement : statements) {
		// a CTE is a write only when an UPDATE follows it; DuckLake's stats reads are CTEs too
		const bool cte_update =
		    StringUtil::StartsWith(statement, "WITH ") && statement.find(update_head) != string::npos;
		if (!StringUtil::StartsWith(statement, update_head) && !StringUtil::StartsWith(statement, delete_head) &&
		    !StringUtil::StartsWith(statement, drop_head) && !cte_update) {
			return nullptr;
		}
		string tsql, dropped;
		if (RewriteUpdate(statement, schema, tsql) || RewriteDelete(statement, schema, tsql) ||
		    RewriteCteUpdate(statement, schema, tsql)) {
			pieces.push_back(tsql + "\n");
			continue;
		}
		if (RewriteDropIfExists(statement, schema, tsql, dropped)) {
			pieces.push_back(tsql + "\n");
			dropped_in_run.push_back(dropped);
			continue;
		}
		if (StrictBatchEnabled()) {
			throw InvalidInputException(
			    "mssql_ducklake: a write DuckLake sent through Query that the T-SQL rewrite does not "
			    "recognise (specs/014): %s",
			    statement);
		}
		return nullptr;
	}
	// every statement recognised before any runs; then in calls of a bounded size
	unique_ptr<QueryResult> result;
	string run;
	for (idx_t i = 0; i < pieces.size(); i++) {
		run += pieces[i];
		if (i + 1 == pieces.size() || run.size() + pieces[i + 1].size() > RunLimitBytes()) {
			result = RunCommitBatch(run);
			run.clear();
			if (result->HasError()) {
				return result;
			}
		}
	}
	if (!result->HasError()) {
		for (auto &table : dropped_in_run) {
			InvalidateTableCache(table);
		}
	}
	return result;
}

unique_ptr<QueryResult> MSSQLMetadataManager::Execute(DuckLakeSnapshot snapshot, string &query) {
	EnsureReady();
	// The snapshot's numbers first, so that a literal is a literal; {METADATA_CATALOG} stays until
	// each family decides what to do with it. The base substitutes again on what it is handed and
	// finds nothing left to replace.
	SubstituteSnapshotPlaceholders(snapshot, query);
	const auto schema = SchemaIdentifier();
	// The kinds come from the catalog's own format, so a 1.0 catalog keeps the T-SQL batch it has
	// today and the strict guard holds at both formats. A future format that reports v1.1 metadata
	// but writes wider tuples is declined by width - and named by the guard, which is the re-audit
	// a ducklake bump asks for.
	const bool v1_1 = transaction.GetCatalog().SupportsV1_1Metadata();
	const bool strict = StrictBatchEnabled();
	const bool rewrite = BatchRewriteEnabled();

	string run;
	vector<string> dropped_in_run;
	unique_ptr<QueryResult> last;
	bool failed = false;
	auto flush = [&]() -> bool {
		if (run.empty()) {
			return true;
		}
		last = RunCommitBatch(run);
		run.clear();
		if (last->HasError()) {
			return false;
		}
		for (auto &table : dropped_in_run) {
			InvalidateTableCache(table);
		}
		dropped_in_run.clear();
		return true;
	};
	// the run grows statement by statement and goes as a call of its own past the limit
	auto append = [&](const string &tsql) {
		if (!run.empty() && run.size() + tsql.size() > RunLimitBytes() && !flush()) {
			failed = true;
			return;
		}
		run += tsql;
	};

	// the per-column stats refreshes of one table, gathered while they come one after another
	vector<ColumnStatsRefresh> refreshes;
	auto close_refreshes = [&]() {
		if (!refreshes.empty()) {
			append(CoalescedColumnStatsRefresh(schema, refreshes));
			refreshes.clear();
		}
	};
	for (auto &stmt : SplitStatements(query)) {
		ColumnStatsRefresh refresh;
		if (rewrite && ReadColumnStatsRefresh(stmt, refresh)) {
			BoundRefresh(refresh, stats_length);
			bool joins = !refreshes.empty() && refreshes[0].table_id == refresh.table_id &&
			             refreshes[0].exactness == refresh.exactness;
			// a column twice is two updates in order, the later one winning: not one join
			for (auto &earlier : refreshes) {
				joins = joins && earlier.column_id != refresh.column_id;
			}
			if (!joins) {
				close_refreshes();
				if (failed) {
					return last;
				}
			}
			refreshes.push_back(std::move(refresh));
			continue;
		}
		close_refreshes();
		if (failed) {
			return last;
		}
		// the user's inlined rows, rendered by our WriteNewInlinedData
		if (StringUtil::StartsWith(stmt, INLINED_ROWS_MARKER)) {
			auto index = std::stoull(stmt.substr(strlen(INLINED_ROWS_MARKER)));
			if (index >= inlined_rows_statements.size()) {
				throw InternalException("mssql_ducklake: inlined rows %llu written by no WriteNewInlinedData", index);
			}
			auto &statement = inlined_rows_statements[index];
			string rows_sql = statement.head;
			for (idx_t i = 0; i < statement.rows.size(); i++) {
				rows_sql += StringUtil::Format("%s(%lld, %llu, NULL%s)", i == 0 ? "" : ", ", statement.rows[i].first,
				                               snapshot.snapshot_id, statement.rows[i].second);
			}
			append(rows_sql + ";\n");
			if (failed) {
				return last;
			}
			continue;
		}
		// the one DDL in the batch: the inlined deletion table, created keyed and outside the
		// transaction instead (specs/006 D5b); the loop writes it into every batch that deletes
		// inline from the table, so the catalog-level cache says whether there is anything to do
		const string ddl_head = INLINED_DELETE_DDL_HEAD;
		if (StringUtil::StartsWith(stmt, ddl_head)) {
			idx_t pos = ddl_head.size();
			auto digits = ReadIdentifier(stmt, pos);
			if (!digits.empty() && stmt.compare(pos, string::npos, INLINED_DELETE_DDL_TAIL) == 0) {
				auto table_id = TableIndex(std::stoull(digits));
				auto &catalog = transaction.GetCatalog();
				if (catalog.CheckInlinedDeletionTableCache(table_id, snapshot) != InlinedDeletionCacheResult::EXISTS) {
					CreateInlinedDeletionTable(InlinedFileDeletionTableName(table_id));
					catalog.CacheInlinedDeletionTableResult(table_id, snapshot, true);
				}
				continue;
			}
		}
		string tsql, dropped;
		if (rewrite && (RewriteInsert(stmt, schema, v1_1, stats_length, tsql) || RewriteUpdate(stmt, schema, tsql) ||
		                RewriteDelete(stmt, schema, tsql) || RewriteCteUpdate(stmt, schema, tsql))) {
			append(tsql + "\n");
			if (failed) {
				return last;
			}
			continue;
		}
		if (rewrite && RewriteDropIfExists(stmt, schema, tsql, dropped)) {
			// appended first: a run cut here sends the earlier drops with their own invalidation
			append(tsql + "\n");
			if (failed) {
				return last;
			}
			dropped_in_run.push_back(dropped);
			continue;
		}
		// not on the list: the user's inlined rows by design, anything else by omission - which
		// the strict switch turns into an error so that the suite names it
		if (strict && rewrite && !IsInlinedRowsInsert(stmt) && stmt.find(CATALOG_PREFIX) != string::npos) {
			throw InvalidInputException("mssql_ducklake: a commit statement the T-SQL batch does not recognise "
			                            "(specs/014): %s",
			                            stmt);
		}
		if (!flush()) {
			return last;
		}
		string one = stmt + ";";
		last = DuckLakeMetadataManager::Execute(snapshot, one);
		if (last->HasError()) {
			return last;
		}
	}
	close_refreshes();
	if (failed || !flush()) {
		return last;
	}
	if (!last) {
		// nothing ran - every statement was the DDL above; the base would refuse an empty batch
		last = TracedQuery(transaction.GetConnection(), "SELECT 1");
	}
	return last;
}

} // namespace duckdb
