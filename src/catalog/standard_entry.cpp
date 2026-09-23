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
	auto schema = catalog.GetSchema(transaction, schema_info->Name(), OnEntryNotFound::RETURN_NULL);
	if (schema && schema->GetSchemaInfo() == schema_info) {
		return *schema;
	}
	optional_ptr<SchemaCatalogEntry> found;
	if (transaction.context) {
		catalog.ScanSchemas(*transaction.context, [&](SchemaCatalogEntry &candidate) {
			if (candidate.GetSchemaInfo() == schema_info) {
				found = &candidate;
			}
		});
	}
	if (found) {
		return *found;
	}
	return catalog.GetSchema(transaction, schema_info->Name());
}

} // namespace duckdb
