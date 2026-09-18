#include "duckdb/catalog/standard_entry.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"

namespace duckdb {

StandardEntry::StandardEntry(CatalogType type, SchemaCatalogEntry &schema, Catalog &catalog, Identifier name,
                             idx_t oid)
    : InCatalogEntry(type, catalog, std::move(name), oid), schema_info(schema.GetSchemaInfo()) {
}

SchemaCatalogEntry &StandardEntry::ParentSchema(CatalogTransaction transaction) const {
	return catalog.GetSchema(transaction, schema_info->name);
}

} // namespace duckdb
