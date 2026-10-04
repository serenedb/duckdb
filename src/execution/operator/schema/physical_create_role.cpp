#include "duckdb/execution/operator/schema/physical_create_role.hpp"

#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

SourceResultType PhysicalCreateRole::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                     OperatorSourceInput &input) const {
	auto &catalog = Catalog::GetCatalog(context.client, info->GetQualifiedName().Catalog());
	catalog.CreateRole(catalog.GetCatalogTransaction(context.client), *info);
	return SourceResultType::FINISHED;
}

} // namespace duckdb
