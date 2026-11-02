#include "duckdb/catalog/catalog_entry/function_entry.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

FunctionEntry::FunctionEntry(CatalogType type, Catalog &catalog, SchemaCatalogEntry &schema, CreateFunctionInfo &info)
    : StandardEntry(type, schema, catalog, info.GetFunctionName(), info.oid) {
	descriptions = std::move(info.descriptions);
	alias_of = std::move(info.alias_of);
	this->dependencies = info.dependencies;
	this->internal = info.internal;
	this->extension_name = info.extension_name;
	if (catalog.IsSystemCatalog()) {
		auto &superuser_functions = DBConfig::GetConfig(catalog.GetDatabase()).superuser_functions;
		auto entry = superuser_functions.find(name);
		if (entry != superuser_functions.end()) {
			superuser_only = true;
			superuser_capability = entry->second;
		}
	}
}

} // namespace duckdb
