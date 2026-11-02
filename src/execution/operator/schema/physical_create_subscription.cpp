#include "duckdb/execution/operator/schema/physical_create_subscription.hpp"

#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

SourceResultType PhysicalCreateSubscription::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                             OperatorSourceInput &input) const {
	auto &catalog = Catalog::GetCatalog(context.client, info->GetQualifiedName().Catalog());
	catalog.CreateSubscription(catalog.GetCatalogTransaction(context.client), *info);
	return SourceResultType::FINISHED;
}

} // namespace duckdb
