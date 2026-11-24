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

protected:
	vector<DefaultAcl> default_acls;
};

} // namespace duckdb
