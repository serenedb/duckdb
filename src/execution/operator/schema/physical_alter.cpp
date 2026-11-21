#include "duckdb/execution/operator/schema/physical_alter.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/alter_database_info.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
SourceResultType PhysicalAlter::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                OperatorSourceInput &input) const {
	for (auto &info : infos) {
		if (info->type == AlterType::ALTER_DATABASE) {
			DatabaseManager::Get(context.client).Alter(context.client, info->Cast<AlterDatabaseInfo>());
			continue;
		}
		Catalog::GetDDLCatalog(context.client, info->GetCatalogType(), info->GetQualifiedName().Catalog())
		    .Alter(context.client, *info);
	}
	return SourceResultType::FINISHED;
}

} // namespace duckdb
