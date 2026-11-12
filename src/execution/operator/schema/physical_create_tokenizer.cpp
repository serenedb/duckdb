#include "duckdb/execution/operator/schema/physical_create_tokenizer.hpp"

#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

SourceResultType PhysicalCreateTokenizer::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                          OperatorSourceInput &input) const {
	auto &catalog = Catalog::GetCatalog(context.client, info->GetQualifiedName().Catalog());
	catalog.CreateTokenizer(context.client, *info);
	return SourceResultType::FINISHED;
}

} // namespace duckdb
