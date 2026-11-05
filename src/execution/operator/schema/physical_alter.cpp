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
	if (info->type == AlterType::ALTER_DATABASE) {
		auto &db_info = info->Cast<AlterDatabaseInfo>();
		auto &db_manager = DatabaseManager::Get(context.client);
		db_manager.Alter(context.client, db_info);
		return SourceResultType::FINISHED;
	}
	auto alter = [&](AlterInfo &target) {
		Catalog::GetCatalog(context.client, target.GetQualifiedName().Catalog()).Alter(context.client, target);
	};
	alter(*info);
	if (info->type == AlterType::ALTER_PERMISSIONS) {
		for (auto &grant : info->Cast<AlterPermissionsInfo>().additional_grants) {
			alter(*grant);
		}
	} else if (info->type == AlterType::ALTER_ROLE) {
		for (auto &grant : info->Cast<AlterRoleInfo>().additional_grants) {
			alter(*grant);
		}
	}
	return SourceResultType::FINISHED;
}

} // namespace duckdb
