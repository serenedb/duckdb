#include "duckdb/catalog/catalog_entry/function_entry.hpp"

#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

FunctionEntry::FunctionEntry(CatalogType type, Catalog &catalog, SchemaCatalogEntry &schema, CreateFunctionInfo &info)
    : StandardEntry(type, schema, catalog, info.GetFunctionName(), info.oid) {
	descriptions = std::move(info.descriptions);
	alias_of = std::move(info.alias_of);
	this->dependencies = info.dependencies;
	this->internal = info.internal;
	this->extension_name = info.extension_name;
	required_capability = info.required_capability;
}

} // namespace duckdb
