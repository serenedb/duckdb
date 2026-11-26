#include "duckdb/catalog/catalog_entry/database_entry.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"

namespace duckdb {

DatabaseEntry::DatabaseEntry(Catalog &catalog, CreateDatabaseInfo &info)
    : InCatalogEntry(CatalogType::DATABASE_ENTRY, catalog, info.GetQualifiedName().Name(), info.oid),
      default_acls(std::move(info.permissions.defaults)), settings(std::move(info.settings)) {
	auto &oids = catalog.GetDatabase().GetDatabaseManager();
	for (auto &row : default_acls) {
		row.oid = row.oid ? oids.ClaimOid(row.oid) : oids.NextOid();
	}
}

optional_ptr<const vector<string>> DatabaseEntry::SettingsOf(idx_t role) const {
	for (auto &setting : settings) {
		if (setting.role == role) {
			return &setting.config;
		}
	}
	return nullptr;
}

} // namespace duckdb
