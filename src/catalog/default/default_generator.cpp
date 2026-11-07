#include "duckdb/catalog/default/default_generator.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"

namespace duckdb {

DefaultGenerator::DefaultGenerator(Catalog &catalog) : catalog(catalog), created_all_entries(false) {
}
DefaultGenerator::~DefaultGenerator() {
}

unique_ptr<CatalogEntry> DefaultGenerator::CreateDefaultEntry(ClientContext &context, const Identifier &entry_name) {
	throw InternalException("CreateDefaultEntry with ClientContext called but not supported in this generator");
}

unique_ptr<CatalogEntry> DefaultGenerator::CreateDefaultEntry(CatalogTransaction transaction,
                                                              const Identifier &entry_name) {
	if (!transaction.context) {
		// no context - cannot create default entry
		return nullptr;
	}
	return CreateDefaultEntry(*transaction.context, entry_name);
}

bool DefaultGenerator::ServesDefaults(const Identifier &schema, const Identifier &defaults) const {
	if (schema == defaults) {
		return true;
	}
	auto default_schema = catalog.GetDefaultSchema();
	return defaults == DEFAULT_SCHEMA && default_schema && schema == *default_schema;
}

} // namespace duckdb
