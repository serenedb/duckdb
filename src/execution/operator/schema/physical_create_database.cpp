#include "duckdb/execution/operator/schema/physical_create_database.hpp"

#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

SourceResultType PhysicalCreateDatabase::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                         OperatorSourceInput &input) const {
	auto &catalog = Catalog::GetSystemCatalog(context.client);
	catalog.CreateDatabase(catalog.GetCatalogTransaction(context.client), *info);
	return SourceResultType::FINISHED;
}

} // namespace duckdb
