#include "duckdb/execution/operator/schema/physical_create_foreign_server.hpp"

#include "duckdb/catalog/duck_catalog.hpp"

namespace duckdb {

SourceResultType PhysicalCreateForeignServer::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                              OperatorSourceInput &input) const {
	auto &catalog = Catalog::GetCatalog(context.client, info->GetQualifiedName().Catalog()).Cast<DuckCatalog>();
	catalog.CreateForeignServer(catalog.GetCatalogTransaction(context.client), *info);
	return SourceResultType::FINISHED;
}

} // namespace duckdb
