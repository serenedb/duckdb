#include "duckdb/catalog/standard_entry.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"

namespace duckdb {

StandardEntry::StandardEntry(CatalogType type, SchemaCatalogEntry &schema, Catalog &catalog, Identifier name)
    : InCatalogEntry(type, catalog, std::move(name)), schema_info(schema.GetSchemaInfo()) {
}

SchemaCatalogEntry &StandardEntry::ParentSchema(CatalogTransaction transaction) const {
	auto path = schema_info->Path();
	auto schema = catalog.GetSchema(transaction, path, OnEntryNotFound::RETURN_NULL);
	if (schema && schema->GetSchemaInfo() == schema_info) {
		return *schema;
	}
	optional_ptr<SchemaCatalogEntry> found;
	if (transaction.context) {
		catalog.ScanSchemas(*transaction.context, [&](SchemaCatalogEntry &root) {
			root.ScanSchemaTree(transaction, [&](SchemaCatalogEntry &candidate) {
				if (candidate.GetSchemaInfo() == schema_info) {
					found = &candidate;
				}
			});
		});
	}
	if (found) {
		return *found;
	}
	return *catalog.GetSchema(transaction, path, OnEntryNotFound::THROW_EXCEPTION);
}

QualifiedName StandardEntry::GetQualifiedName(const Identifier &entry_name) const {
	return QualifiedName::FromCatalogSchema(catalog.GetName(), schema_info->Path(), entry_name);
}

} // namespace duckdb
