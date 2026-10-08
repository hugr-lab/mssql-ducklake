//===----------------------------------------------------------------------===//
// The bounded string columns of a DuckLake catalog on SQL Server (specs/018)
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

class AttachedDatabase;
class DBConfig;
class ExtensionLoader;

//! The lengths a catalog's string columns are declared with, in bytes of UTF-8. MAX (0) is
//! VARCHAR(MAX); NOT_GIVEN is "not in META_LIMITS" - a new catalog takes the default then, a
//! migration leaves the column as it is.
struct CatalogLengths {
	static constexpr int64_t NOT_GIVEN = -1;
	static constexpr int64_t MAX = 0;

	int64_t name = NOT_GIVEN;
	int64_t path = NOT_GIVEN;
	int64_t column_type = NOT_GIVEN;
	int64_t default_value = NOT_GIVEN;
	int64_t text = NOT_GIVEN;
	int64_t stats = NOT_GIVEN;

	bool AnyGiven() const;
	//! every length not given set to its default - a new catalog
	CatalogLengths WithDefaults() const;
	//! every length not given set to MAX - a catalog that existed before the lengths did
	CatalogLengths WithMaxForMissing() const;
	//! these, with every length `other` gives replacing this one's
	CatalogLengths OverriddenBy(const CatalogLengths &other) const;
	//! `name_length=256,path_length=1024,...,stats_length=max` - the extended property's value
	string Serialize() const;
	//! the property's value back; a key it does not carry is NOT_GIVEN
	static CatalogLengths Parse(const string &text);
	//! META_LIMITS: a STRUCT or MAP of {'name_length': 256, ..., 'stats_length': 'max'}, or the property's text
	//! form. Throws, naming the key, on one it does not know or a length outside 1-8000/'max'.
	static CatalogLengths FromValue(const Value &value);
	bool operator==(const CatalogLengths &other) const;
	bool operator!=(const CatalogLengths &other) const {
		return !(*this == other);
	}
};

//! What a catalog string column holds, which decides its length.
enum class LengthClass : uint8_t {
	NAME,
	PATH,
	COLUMN_TYPE,
	DEFAULT_VALUE,
	TEXT,
	STATS,
	//! formats, dialects, value types, sort directions, transforms: VARCHAR(64)
	SHORT,
	//! ducklake_inlined_data_<table>_<version>: VARCHAR(128)
	GENERATED_NAME,
	//! encryption keys: VARCHAR(256)
	KEY_MATERIAL,
	//! grows with one commit's objects (ducklake_snapshot_changes.changes_made): MAX
	UNBOUNDED,
};

struct CatalogStringColumn {
	const char *table;
	const char *column;
	LengthClass length_class;
};

//! Every string column of DuckLake's catalog this extension sizes, with what it holds. The key
//! columns the shaping caps at 200 for its primary keys are not here; a column that is in neither
//! - one a DuckLake bump adds - stays MAX.
const vector<CatalogStringColumn> &CatalogStringColumns();

//! The length of a class under these lengths: bytes, or MAX.
int64_t LengthOf(LengthClass length_class, const CatalogLengths &lengths);

//! `VARCHAR(256)` or `VARCHAR(MAX)`.
string VarcharOf(int64_t length);

//! What the ATTACH that created `db` asked for in META_LIMITS (nothing given: there was none).
CatalogLengths RequestedLengths(const AttachedDatabase &db);

//! Wraps DuckLake's ATTACH so that META_LIMITS is taken out of the options on the way in: kept for
//! the attached database, and not forwarded - DuckLake would pass it on to the metadata database's
//! ATTACH, which a STRUCT value breaks.
void WatchDuckLakeAttach(DBConfig &config);

//! mssql_ducklake_catalog_info('lake'): the catalog's recorded limits and markers, the database's
//! options, and every string column as the server declares it.
void RegisterCatalogInfoFunction(ExtensionLoader &loader);

} // namespace duckdb
