//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/database_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_database_info.hpp"

namespace duckdb {

class DatabaseEntry : public InCatalogEntry {
public:
	static constexpr const CatalogType Type = CatalogType::DATABASE_ENTRY;
	static constexpr const char *Name = "database";

	DatabaseEntry(Catalog &catalog, CreateDatabaseInfo &info);

	const vector<DefaultAcl> &DefaultAcls() const {
		return default_acls;
	}
	const vector<DatabaseSetting> &Settings() const {
		return settings;
	}
	optional_ptr<const vector<string>> SettingsOf(idx_t role) const;

protected:
	vector<DefaultAcl> default_acls;
	vector<DatabaseSetting> settings;
};

} // namespace duckdb
