#include "duckdb/execution/operator/persistent/physical_copy_database.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_sequence_info.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_tokenizer_info.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/execution/index/unbound_index.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"

namespace duckdb {

PhysicalCopyDatabase::PhysicalCopyDatabase(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                           idx_t estimated_cardinality, unique_ptr<CopyDatabaseInfo> info_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::COPY_DATABASE, std::move(types), estimated_cardinality),
      info(std::move(info_p)) {
}

PhysicalCopyDatabase::~PhysicalCopyDatabase() {
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
SourceResultType PhysicalCopyDatabase::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                       OperatorSourceInput &input) const {
	auto &catalog = Catalog::GetCatalog(context.client, info->target_database);
	vector<unique_ptr<ChangeOwnershipInfo>> ownerships;
	for (auto &create_info : info->entries) {
		switch (create_info->type) {
		case CatalogType::SCHEMA_ENTRY:
			catalog.CreateSchema(context.client, create_info->Cast<CreateSchemaInfo>());
			break;
		case CatalogType::VIEW_ENTRY:
			catalog.CreateView(context.client, create_info->Cast<CreateViewInfo>());
			break;
		case CatalogType::SEQUENCE_ENTRY: {
			auto &sequence = create_info->Cast<CreateSequenceInfo>();
			if (!sequence.owned_by.empty()) {
				auto ownership = make_uniq<ChangeOwnershipInfo>(
				    CatalogType::SEQUENCE_ENTRY,
				    AlterEntryData(sequence.GetQualifiedName(), OnEntryNotFound::THROW_EXCEPTION), Identifier(),
				    Identifier());
				ownership->owner_path = std::move(sequence.owned_by);
				sequence.owned_by.clear();
				ownerships.push_back(std::move(ownership));
			}
			catalog.CreateSequence(context.client, sequence);
			break;
		}
		case CatalogType::TYPE_ENTRY:
			catalog.CreateType(context.client, create_info->Cast<CreateTypeInfo>());
			break;
		case CatalogType::MACRO_ENTRY:
		case CatalogType::TABLE_MACRO_ENTRY:
			catalog.CreateFunction(context.client, create_info->Cast<CreateMacroInfo>());
			break;
		case CatalogType::TOKENIZER_ENTRY:
			catalog.CreateTokenizer(context.client, create_info->Cast<CreateTokenizerInfo>());
			break;
		case CatalogType::TABLE_ENTRY: {
			auto binder = Binder::CreateBinder(context.client);
			auto bound_info = binder->BindCreateTableInfo(std::move(create_info));
			catalog.CreateTable(context.client, *bound_info);
			break;
		}
		default:
			throw NotImplementedException("Entry type %s not supported in PhysicalCopyDatabase",
			                              CatalogTypeToString(create_info->type));
		}
	}
	for (auto &ownership : ownerships) {
		catalog.Alter(context.client, *ownership);
	}

	return SourceResultType::FINISHED;
}

} // namespace duckdb
