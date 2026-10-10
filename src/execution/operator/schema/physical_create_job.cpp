#include "duckdb/execution/operator/schema/physical_create_job.hpp"

#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

SourceResultType PhysicalCreateJob::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                    OperatorSourceInput &input) const {
	auto &catalog = Catalog::GetCatalog(context.client, info->GetQualifiedName().Catalog());
	catalog.CreateJob(context.client, *info);
	return SourceResultType::FINISHED;
}

} // namespace duckdb
