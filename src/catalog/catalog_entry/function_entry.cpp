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
	superuser_only =
	    catalog.IsSystemCatalog() && DBConfig::GetConfig(catalog.GetDatabase()).superuser_functions.count(name) != 0;
}

} // namespace duckdb
