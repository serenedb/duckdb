#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/execution/operator/persistent/physical_batch_insert.hpp"
#include "duckdb/execution/operator/persistent/physical_insert.hpp"
#include "duckdb/execution/operator/schema/physical_create_table.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/constraints/bound_check_constraint.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"

namespace duckdb {

PhysicalOperator &DuckCatalog::PlanCreateTableAsInsert(ClientContext &context, PhysicalPlanGenerator &planner,
                                                       LogicalCreateTable &op, SchemaCatalogEntry &schema,
                                                       unique_ptr<BoundCreateTableInfo> info, PhysicalOperator &plan,
                                                       idx_t estimated_cardinality) {
	bool parallel_streaming_insert = !PhysicalPlanGenerator::PreserveInsertionOrder(context, plan);
	bool use_batch_index = PhysicalPlanGenerator::UseBatchIndex(context, plan);
	auto num_threads = TaskScheduler::GetScheduler(context).NumberOfThreads();
	if (!parallel_streaming_insert && use_batch_index) {
		return planner.Make<PhysicalBatchInsert>(op, schema, std::move(info), estimated_cardinality);
	}

	auto parallel = parallel_streaming_insert && num_threads > 1;
	return planner.Make<PhysicalInsert>(op, schema, std::move(info), estimated_cardinality, parallel);
}

PhysicalOperator &DuckCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                                 LogicalCreateTable &op, PhysicalOperator &plan) {
	auto &insert = PlanCreateTableAsInsert(context, planner, op, op.schema, std::move(op.info), plan, 0U);
	D_ASSERT(op.children.size() == 1);
	insert.children.push_back(plan);
	return insert;
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalCreateTable &op) {
	const auto &create_info = op.info->base->Cast<CreateTableInfo>();
	auto &catalog = op.info->schema.catalog;
	auto existing_entry = catalog.GetEntry(context, CatalogType::TABLE_ENTRY, create_info.GetQualifiedName().Schema(),
	                                       create_info.GetTableName(), OnEntryNotFound::RETURN_NULL);
	bool replace = op.info->Base().on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT;
	if ((!existing_entry || replace) && !op.children.empty()) {
		auto &plan = CreatePlan(*op.children[0]);
		return op.schema.catalog.PlanCreateTableAs(context, *this, op, plan);
	}
	return Make<PhysicalCreateTable>(op, op.schema, std::move(op.info), op.estimated_cardinality);
}

} // namespace duckdb
