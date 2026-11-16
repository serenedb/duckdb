#include "duckdb/execution/operator/schema/physical_create_tablespace.hpp"

#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

SourceResultType PhysicalCreateTablespace::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                           OperatorSourceInput &input) const {
	auto &catalog = Catalog::GetSystemCatalog(context.client);
	catalog.CreateTablespace(catalog.GetCatalogTransaction(context.client), *info);
	return SourceResultType::FINISHED;
}

} // namespace duckdb
