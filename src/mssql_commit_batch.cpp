#include "mssql_metadata_manager.hpp"
#include "mssql_metadata_internal.hpp"

#include "common/ducklake_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"
#include "storage/ducklake_catalog.hpp"
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
const unordered_map<string, string> &CatalogColumns() {
	static const unordered_map<string, string> columns = {
	    {"ducklake_metadata", "sssn"},
	    {"ducklake_snapshot", "ntnnn"},
	    {"ducklake_snapshot_changes", "nssss"},
	    {"ducklake_schema", "nunnssb"},
	    {"ducklake_table", "nunnnssb"},
	    {"ducklake_view", "nunnnssss"},
	    {"ducklake_tag", "nnnss"},
	    {"ducklake_column_tag", "nnnnss"},
	    {"ducklake_data_file", "nnnnnsbsnnnnnsnn"},
	    {"ducklake_file_column_stats", "nnnnnnssbs"},
	    {"ducklake_file_variant_stats", "nnnssnnnssbs"},
	    {"ducklake_delete_file", "nnnnnsbsnnnsn"},
	    {"ducklake_column", "nnnnnssssbnss"},
	    {"ducklake_table_stats", "nnnn"},
	    {"ducklake_table_column_stats", "nnbbsss"},
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
bool RewriteInsert(const string &stmt, const string &schema, string &tsql) {
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
		auto entry = CatalogColumns().find(table);
		if (entry == CatalogColumns().end()) {
			return false;
		}
		kinds = entry->second;
	}
	SkipSpace(stmt, pos);
	auto keyword = ReadIdentifier(stmt, pos);
	if (StringUtil::Upper(keyword) != "VALUES") {
		return false;
	}
	vector<vector<Literal>> tuples;
	if (!ReadTuples(stmt, pos, tuples) || tuples.empty()) {
		return false;
	}
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
	tsql = ChunkedStatements("INSERT INTO " + schema + "." + table + " VALUES ", rows, ";");
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
		if (IsIdentifierChar(c)) {
			auto word = ReadIdentifier(body, pos);
			auto next = pos;
			SkipSpace(body, next);
			if (next < body.size() && body[next] == '(') {
				// the calls these families carry: IN (...), NOT EXISTS (...), and the CTE updates'
				// CAST(x AS BIT), already rewritten from BOOLEAN by the caller
				auto upper = StringUtil::Upper(word);
				if (upper != "IN" && upper != "EXISTS" && upper != "CAST") {
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

bool InlinedDeletionDdlIsDuckLakes() {
	DuckLakeInlinedFileDeletionInfo probe;
	probe.table_id = TableIndex(7);
	vector<DuckLakeInlinedFileDeletionInfo> one;
	one.push_back(std::move(probe));
	auto generated = DuckLakeMetadataManager::WriteNewInlinedFileDeletesSqlBatch(one);
	return StringUtil::StartsWith(generated, string(INLINED_DELETE_DDL_HEAD) + "7" + INLINED_DELETE_DDL_TAIL + ";");
}

unique_ptr<QueryResult> MSSQLMetadataManager::RunCommitBatch(const string &tsql) {
	auto &connection = transaction.GetConnection();
	return connection.Query(StringUtil::Format("SELECT mssql_exec(%s, %s)", CatalogLiteral(), SQLString(tsql)));
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
	string run;
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
			run += tsql + "\n";
			continue;
		}
		if (RewriteDropIfExists(statement, schema, tsql, dropped)) {
			run += tsql + "\n";
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
	auto result = RunCommitBatch(run);
	if (!result->HasError()) {
		for (auto &table : dropped_in_run) {
			InvalidateTableCache(table);
		}
	}
	return result;
}

unique_ptr<QueryResult> MSSQLMetadataManager::Execute(DuckLakeSnapshot snapshot, string &query) {
	// The snapshot's numbers first, so that a literal is a literal; {METADATA_CATALOG} stays until
	// each family decides what to do with it. The base substitutes again on what it is handed and
	// finds nothing left to replace.
	SubstituteSnapshotPlaceholders(snapshot, query);
	const auto schema = SchemaIdentifier();
	const bool strict = StrictBatchEnabled();
	const bool rewrite = BatchRewriteEnabled();

	string run;
	vector<string> dropped_in_run;
	unique_ptr<QueryResult> last;
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

	for (auto &stmt : SplitStatements(query)) {
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
		if (rewrite && (RewriteInsert(stmt, schema, tsql) || RewriteUpdate(stmt, schema, tsql) ||
		                RewriteDelete(stmt, schema, tsql) || RewriteCteUpdate(stmt, schema, tsql))) {
			run += tsql + "\n";
			continue;
		}
		if (rewrite && RewriteDropIfExists(stmt, schema, tsql, dropped)) {
			run += tsql + "\n";
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
	if (!flush()) {
		return last;
	}
	if (!last) {
		// nothing ran - every statement was the DDL above; the base would refuse an empty batch
		last = transaction.GetConnection().Query("SELECT 1");
	}
	return last;
}

} // namespace duckdb
