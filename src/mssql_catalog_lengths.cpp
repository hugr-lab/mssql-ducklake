#include "mssql_catalog_lengths.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "common/ducklake_util.hpp"
#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_transaction.hpp"

#include <mutex>

namespace duckdb {

//===--------------------------------------------------------------------===//
// The lengths (specs/018)
//===--------------------------------------------------------------------===//

namespace {

struct LengthKey {
	const char *key;
	int64_t CatalogLengths::*field;
	int64_t default_length;
};

//! the keys of META_LIMITS, their fields and a new catalog's defaults (0 = MAX)
const LengthKey LENGTH_KEYS[] = {
    {"name_length", &CatalogLengths::name, 256},
    {"path_length", &CatalogLengths::path, 1024},
    {"column_type_length", &CatalogLengths::column_type, 1024},
    {"default_length", &CatalogLengths::default_value, 1024},
    {"text_length", &CatalogLengths::text, 2048},
    {"stats_length", &CatalogLengths::stats, 1024},
};

constexpr int64_t LONGEST = 8000;

string KnownKeys() {
	string out;
	for (auto &entry : LENGTH_KEYS) {
		out += (out.empty() ? "" : ", ") + string(entry.key);
	}
	return out;
}

int64_t LengthFromValue(const string &key, const Value &value) {
	if (value.IsNull()) {
		throw InvalidInputException("META_LIMITS: '%s' is NULL; give a length in bytes (1-%lld) or 'max'", key,
		                            LONGEST);
	}
	if (value.type().id() == LogicalTypeId::VARCHAR) {
		auto text = StringUtil::Lower(StringValue::Get(value));
		StringUtil::Trim(text);
		if (text == "max") {
			return CatalogLengths::MAX;
		}
		int64_t parsed;
		if (!TryCast::Operation(string_t(text), parsed, false)) {
			throw InvalidInputException("META_LIMITS: '%s' is '%s'; give a length in bytes (1-%lld) or 'max'", key,
			                            text, LONGEST);
		}
		return LengthFromValue(key, Value::BIGINT(parsed));
	}
	int64_t length;
	try {
		length = value.GetValue<int64_t>();
	} catch (std::exception &) {
		throw InvalidInputException("META_LIMITS: '%s' is %s; give a length in bytes (1-%lld) or 'max'", key,
		                            value.ToString(), LONGEST);
	}
	if (length < 0 || length > LONGEST) {
		throw InvalidInputException("META_LIMITS: '%s' is %lld; a length is 1-%lld bytes, or 0 / 'max' for MAX", key,
		                            length, LONGEST);
	}
	return length;
}

void SetLength(CatalogLengths &lengths, const string &key, const Value &value) {
	auto lower = StringUtil::Lower(key);
	for (auto &entry : LENGTH_KEYS) {
		if (lower == entry.key) {
			lengths.*entry.field = LengthFromValue(lower, value);
			return;
		}
	}
	throw InvalidInputException("META_LIMITS: unknown key '%s' (the keys are %s)", key, KnownKeys());
}

} // namespace

bool CatalogLengths::AnyGiven() const {
	for (auto &entry : LENGTH_KEYS) {
		if (this->*entry.field != NOT_GIVEN) {
			return true;
		}
	}
	return false;
}

CatalogLengths CatalogLengths::WithDefaults() const {
	auto out = *this;
	for (auto &entry : LENGTH_KEYS) {
		if (out.*entry.field == NOT_GIVEN) {
			out.*entry.field = entry.default_length;
		}
	}
	return out;
}

CatalogLengths CatalogLengths::WithMaxForMissing() const {
	auto out = *this;
	for (auto &entry : LENGTH_KEYS) {
		if (out.*entry.field == NOT_GIVEN) {
			out.*entry.field = MAX;
		}
	}
	return out;
}

CatalogLengths CatalogLengths::OverriddenBy(const CatalogLengths &other) const {
	auto out = *this;
	for (auto &entry : LENGTH_KEYS) {
		if (other.*entry.field != NOT_GIVEN) {
			out.*entry.field = other.*entry.field;
		}
	}
	return out;
}

string CatalogLengths::Serialize() const {
	string out;
	for (auto &entry : LENGTH_KEYS) {
		auto length = this->*entry.field;
		if (length == NOT_GIVEN) {
			continue;
		}
		out += StringUtil::Format("%s%s=%s", out.empty() ? "" : ",", entry.key,
		                          length == MAX ? string("max") : to_string(length));
	}
	return out;
}

CatalogLengths CatalogLengths::Parse(const string &text) {
	CatalogLengths lengths;
	for (auto &pair : StringUtil::Split(text, ',')) {
		auto eq = pair.find('=');
		if (eq == string::npos) {
			throw InvalidInputException("META_LIMITS: '%s' is not key=length", pair);
		}
		auto key = pair.substr(0, eq);
		StringUtil::Trim(key);
		SetLength(lengths, key, Value(pair.substr(eq + 1)));
	}
	return lengths;
}

CatalogLengths CatalogLengths::FromValue(const Value &value) {
	CatalogLengths lengths;
	switch (value.type().id()) {
	case LogicalTypeId::STRUCT: {
		auto &names = StructType::GetChildTypes(value.type());
		auto &children = StructValue::GetChildren(value);
		for (idx_t i = 0; i < children.size(); i++) {
			SetLength(lengths, names[i].first.GetIdentifierName(), children[i]);
		}
		return lengths;
	}
	case LogicalTypeId::MAP:
		for (auto &entry : MapValue::GetChildren(value)) {
			auto &key_value = StructValue::GetChildren(entry);
			SetLength(lengths, key_value[0].ToString(), key_value[1]);
		}
		return lengths;
	case LogicalTypeId::VARCHAR:
		return Parse(StringValue::Get(value));
	default:
		throw InvalidInputException("META_LIMITS is a struct of lengths, e.g. META_LIMITS {'name_length': 256, "
		                            "'path_length': 1024, 'stats_length': 'max'}");
	}
}

bool CatalogLengths::operator==(const CatalogLengths &other) const {
	for (auto &entry : LENGTH_KEYS) {
		if (this->*entry.field != other.*entry.field) {
			return false;
		}
	}
	return true;
}

//===--------------------------------------------------------------------===//
// What each column holds
//===--------------------------------------------------------------------===//

const vector<CatalogStringColumn> &CatalogStringColumns() {
	using C = LengthClass;
	static const vector<CatalogStringColumn> columns = {
	    {"ducklake_schema", "schema_name", C::NAME},
	    {"ducklake_schema", "path", C::PATH},
	    {"ducklake_table", "table_name", C::NAME},
	    {"ducklake_table", "path", C::PATH},
	    {"ducklake_view", "view_name", C::NAME},
	    {"ducklake_view", "dialect", C::SHORT},
	    {"ducklake_view", "sql", C::TEXT},
	    {"ducklake_view", "column_aliases", C::TEXT},
	    {"ducklake_column", "column_name", C::NAME},
	    {"ducklake_column", "column_type", C::COLUMN_TYPE},
	    {"ducklake_column", "initial_default", C::DEFAULT_VALUE},
	    {"ducklake_column", "default_value", C::DEFAULT_VALUE},
	    {"ducklake_column", "default_value_type", C::SHORT},
	    {"ducklake_column", "default_value_dialect", C::SHORT},
	    {"ducklake_tag", "value", C::TEXT},
	    {"ducklake_column_tag", "value", C::TEXT},
	    {"ducklake_view_column_tag", "value", C::TEXT},
	    {"ducklake_data_file", "path", C::PATH},
	    {"ducklake_data_file", "file_format", C::SHORT},
	    {"ducklake_data_file", "encryption_key", C::KEY_MATERIAL},
	    {"ducklake_delete_file", "path", C::PATH},
	    {"ducklake_delete_file", "format", C::SHORT},
	    {"ducklake_delete_file", "encryption_key", C::KEY_MATERIAL},
	    {"ducklake_files_scheduled_for_deletion", "path", C::PATH},
	    {"ducklake_file_column_stats", "min_value", C::STATS},
	    {"ducklake_file_column_stats", "max_value", C::STATS},
	    {"ducklake_file_column_stats", "extra_stats", C::TEXT},
	    {"ducklake_table_column_stats", "min_value", C::STATS},
	    {"ducklake_table_column_stats", "max_value", C::STATS},
	    {"ducklake_table_column_stats", "extra_stats", C::TEXT},
	    {"ducklake_file_variant_stats", "shredded_type", C::SHORT},
	    {"ducklake_file_variant_stats", "min_value", C::STATS},
	    {"ducklake_file_variant_stats", "max_value", C::STATS},
	    {"ducklake_file_variant_stats", "extra_stats", C::TEXT},
	    {"ducklake_inlined_data_tables", "table_name", C::GENERATED_NAME},
	    {"ducklake_metadata", "value", C::TEXT},
	    {"ducklake_metadata", "scope", C::SHORT},
	    {"ducklake_snapshot_changes", "changes_made", C::UNBOUNDED},
	    {"ducklake_snapshot_changes", "author", C::TEXT},
	    {"ducklake_snapshot_changes", "commit_message", C::TEXT},
	    {"ducklake_snapshot_changes", "commit_extra_info", C::TEXT},
	    {"ducklake_macro", "macro_name", C::NAME},
	    {"ducklake_macro_impl", "dialect", C::SHORT},
	    {"ducklake_macro_impl", "sql", C::TEXT},
	    {"ducklake_macro_impl", "type", C::SHORT},
	    {"ducklake_macro_parameters", "parameter_name", C::NAME},
	    {"ducklake_macro_parameters", "parameter_type", C::COLUMN_TYPE},
	    {"ducklake_macro_parameters", "default_value", C::DEFAULT_VALUE},
	    {"ducklake_macro_parameters", "default_value_type", C::SHORT},
	    {"ducklake_partition_column", "transform", C::SHORT},
	    {"ducklake_sort_expression", "expression", C::DEFAULT_VALUE},
	    {"ducklake_sort_expression", "dialect", C::SHORT},
	    {"ducklake_sort_expression", "sort_direction", C::SHORT},
	    {"ducklake_sort_expression", "null_order", C::SHORT},
	    {"ducklake_column_mapping", "type", C::COLUMN_TYPE},
	    {"ducklake_name_mapping", "source_name", C::NAME},
	};
	return columns;
}

int64_t LengthOf(LengthClass length_class, const CatalogLengths &lengths) {
	auto given = [](int64_t length) {
		return length == CatalogLengths::NOT_GIVEN ? CatalogLengths::MAX : length;
	};
	switch (length_class) {
	case LengthClass::NAME:
		return given(lengths.name);
	case LengthClass::PATH:
		return given(lengths.path);
	case LengthClass::COLUMN_TYPE:
		return given(lengths.column_type);
	case LengthClass::DEFAULT_VALUE:
		return given(lengths.default_value);
	case LengthClass::TEXT:
		return given(lengths.text);
	case LengthClass::STATS:
		return given(lengths.stats);
	case LengthClass::SHORT:
		return 64;
	case LengthClass::GENERATED_NAME:
		return 128;
	case LengthClass::KEY_MATERIAL:
		return 256;
	case LengthClass::UNBOUNDED:
		return CatalogLengths::MAX;
	}
	return CatalogLengths::MAX;
}

string VarcharOf(int64_t length) {
	return length == CatalogLengths::MAX ? string("VARCHAR(MAX)") : StringUtil::Format("VARCHAR(%lld)", length);
}

//===--------------------------------------------------------------------===//
// META_LIMITS, taken out of DuckLake's ATTACH
//===--------------------------------------------------------------------===//

namespace {

std::mutex requested_lock;
unordered_map<const AttachedDatabase *, CatalogLengths> &Requested() {
	static unordered_map<const AttachedDatabase *, CatalogLengths> requested;
	return requested;
}

//! DuckLake's own handler, called after META_LIMITS is out
attach_function_t ducklake_attach = nullptr;

unique_ptr<Catalog> AttachWithLimits(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                     AttachedDatabase &db, const string &name, AttachInfo &info,
                                     AttachOptions &options) {
	CatalogLengths lengths;
	bool on_mssql = StringUtil::StartsWith(StringUtil::Lower(info.path), "mssql:");
	bool native_types_given = false;
	for (auto it = options.options.begin(); it != options.options.end();) {
		auto key = StringUtil::Lower(it->first);
		if (key == "meta_type") {
			on_mssql = StringUtil::Lower(it->second.ToString()) == "mssql";
		} else if (key == "meta_native_types") {
			native_types_given = true;
		} else if (key == "metadata_parameters" && it->second.type().id() == LogicalTypeId::MAP) {
			for (auto &child : MapValue::GetChildren(it->second)) {
				auto &key_value = StructValue::GetChildren(child);
				auto parameter = StringUtil::Lower(key_value[0].ToString());
				native_types_given = native_types_given || parameter == "native_types";
				if (parameter == "type") {
					on_mssql = StringUtil::Lower(key_value[1].ToString()) == "mssql";
				}
			}
		}
		if (key == "meta_limits") {
			lengths = CatalogLengths::FromValue(it->second);
			it = options.options.erase(it);
		} else {
			++it;
		}
	}
	// The catalog's bounded strings as plain VARCHAR: under the mssql extension's native types they
	// arrive as MSSQL_VARCHAR(n), and DuckLake reads each catalog row with GetValue<string>, which
	// for a type that is not VARCHAR is a full cast per value - a third of the client's time on a
	// commit (specs/018). Only for this catalog's metadata database (mssql #416); META_NATIVE_TYPES
	// or a native_types metadata parameter given at the ATTACH wins.
	if (on_mssql && !native_types_given) {
		options.options["meta_native_types"] = Value::BOOLEAN(false);
	}
	{
		// the same AttachedDatabase the manager meets as its catalog's GetAttached(); a re-attach under
		// the same address replaces the entry
		std::lock_guard<std::mutex> guard(requested_lock);
		Requested()[&db] = lengths;
	}
	return ducklake_attach(storage_info, context, db, name, info, options);
}

} // namespace

CatalogLengths RequestedLengths(const AttachedDatabase &db) {
	std::lock_guard<std::mutex> guard(requested_lock);
	auto &requested = Requested();
	auto entry = requested.find(&db);
	return entry == requested.end() ? CatalogLengths() : entry->second;
}

void WatchDuckLakeAttach(DBConfig &config) {
	auto storage = StorageExtension::Find(config, "ducklake");
	if (!storage) {
		throw InternalException("mssql_ducklake: the embedded ducklake registered no storage extension");
	}
	if (storage->attach == AttachWithLimits) {
		return;
	}
	ducklake_attach = storage->attach;
	storage->attach = AttachWithLimits;
}

} // namespace duckdb

//===--------------------------------------------------------------------===//
// mssql_ducklake_catalog_info('lake')
//===--------------------------------------------------------------------===//

namespace duckdb {

namespace {

unique_ptr<FunctionData> CatalogInfoBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto &ducklake_catalog = catalog.Cast<DuckLakeCatalog>();
	if (ducklake_catalog.MetadataType() != "mssql") {
		throw InvalidInputException("mssql_ducklake_catalog_info: '%s' is not a DuckLake catalog on SQL Server",
		                            input.inputs[0].ToString());
	}
	for (auto name : {"kind", "name", "value"}) {
		names.emplace_back(name);
		return_types.emplace_back(LogicalType::VARCHAR);
	}

	auto schema = ducklake_catalog.MetadataSchemaName().GetIdentifierName();
	auto schema_literal = StringUtil::Replace(schema, "'", "''");
	// one statement: the catalog's own markers, the database's options, and every string column as
	// the server declares it - the truth about the catalog, whatever was asked for at its ATTACH
	auto tsql = StringUtil::Format(R"(
SELECT CAST(N'property' AS NVARCHAR(16)) AS kind, CAST(p.name AS NVARCHAR(256)) AS name, CAST(p.value AS NVARCHAR(400)) AS value
FROM sys.extended_properties p
WHERE p.class = 1 AND p.major_id = OBJECT_ID(QUOTENAME(N'%s') + N'.ducklake_metadata') AND p.minor_id = 0 AND p.name LIKE N'mssql[_]ducklake[_]%%'
UNION ALL SELECT N'database', N'parameterization_forced', CAST(d.is_parameterization_forced AS NVARCHAR(400)) FROM sys.databases d WHERE d.database_id = DB_ID()
UNION ALL SELECT N'database', N'auto_update_statistics_async', CAST(d.is_auto_update_stats_async_on AS NVARCHAR(400)) FROM sys.databases d WHERE d.database_id = DB_ID()
UNION ALL SELECT N'column', CAST(t.name + N'.' + c.name AS NVARCHAR(256)),
       CAST(UPPER(ty.name) + N'(' + CASE WHEN c.max_length = -1 THEN N'MAX' WHEN ty.name LIKE N'n%%' THEN CAST(c.max_length / 2 AS NVARCHAR(10)) ELSE CAST(c.max_length AS NVARCHAR(10)) END + N')' AS NVARCHAR(400))
FROM sys.columns c JOIN sys.tables t ON t.object_id = c.object_id JOIN sys.types ty ON ty.user_type_id = c.user_type_id
WHERE t.schema_id = SCHEMA_ID(N'%s') AND t.name LIKE N'ducklake[_]%%' AND t.name NOT LIKE N'ducklake[_]inlined[_]data[_][0-9]%%'
  AND t.name NOT LIKE N'ducklake[_]inlined[_]delete[_][0-9]%%' AND ty.name IN (N'varchar', N'nvarchar'))",
	                               schema_literal, schema_literal);
	auto &transaction = DuckLakeTransaction::Get(context, ducklake_catalog);
	auto query = StringUtil::Format("SELECT * FROM mssql_scan_unsafe(%s, %s, columns := {'kind': 'VARCHAR', 'name': "
	                                "'VARCHAR', 'value': 'VARCHAR'}) ORDER BY kind DESC, name",
	                                DuckLakeUtil::SQLLiteralToString(ducklake_catalog.MetadataDatabaseName()),
	                                DuckLakeUtil::SQLLiteralToString(tsql));
	auto result = transaction.GetConnection().Query(query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("mssql_ducklake_catalog_info: ");
	}
	auto bind = make_uniq<MetadataBindData>();
	while (auto chunk = result->Fetch()) {
		for (idx_t row = 0; row < chunk->size(); row++) {
			auto kind = chunk->GetValue(0, row).ToString();
			auto name = chunk->GetValue(1, row).ToString();
			auto value = chunk->GetValue(2, row);
			if (kind == "property" && name == "mssql_ducklake_limits" && !value.IsNull()) {
				// the recorded limits one key a row, the way META_LIMITS gives them
				auto lengths = CatalogLengths::Parse(value.ToString());
				for (auto &entry : LENGTH_KEYS) {
					auto length = lengths.*entry.field;
					if (length == CatalogLengths::NOT_GIVEN) {
						continue;
					}
					bind->rows.push_back({Value("limit"), Value(entry.key),
					                      Value(length == CatalogLengths::MAX ? string("max") : to_string(length))});
				}
				continue;
			}
			bind->rows.push_back({Value(kind), Value(name), value});
		}
	}
	return std::move(bind);
}

class CatalogInfoFunction : public DuckLakeBaseMetadataFunction {
public:
	CatalogInfoFunction() : DuckLakeBaseMetadataFunction("mssql_ducklake_catalog_info", CatalogInfoBind) {
	}
};

} // namespace

void RegisterCatalogInfoFunction(ExtensionLoader &loader) {
	loader.RegisterFunction(CatalogInfoFunction());
}

} // namespace duckdb
