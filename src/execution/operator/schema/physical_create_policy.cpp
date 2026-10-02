#include "duckdb/execution/operator/schema/physical_create_policy.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/row_security.hpp"
#include "duckdb/catalog/standard_entry.hpp"

namespace duckdb {

SourceResultType PhysicalCreatePolicy::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                       OperatorSourceInput &input) const {
	auto &qualified_name = info->GetQualifiedName();
	auto &relation = Catalog::GetEntry(context.client,
	                                   EntryLookupInfo(CatalogType::TABLE_ENTRY,
	                                                   QualifiedName(qualified_name.Catalog(), qualified_name.Schema(),
	                                                                 info->base_table->GetQualifiedName().Name())))
	                     .Cast<StandardEntry>();
	auto transaction = relation.ParentCatalog().GetCatalogTransaction(context.client);
	RowSecurity::Get(relation)->CreatePolicy(transaction, relation, *info);
	return SourceResultType::FINISHED;
}

} // namespace duckdb
