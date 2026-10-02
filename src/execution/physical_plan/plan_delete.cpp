#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/execution/operator/persistent/physical_delete.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/constraints/bound_foreign_key_constraint.hpp"
#include "duckdb/catalog/duck_catalog.hpp"

namespace duckdb {

static void DropTruncatedForeignKeys(ClientContext &context, LogicalDelete &op) {
	if (!op.is_truncate || op.table.ParentCatalog().Compatibility() != SqlCompatibility::POSTGRES) {
		return;
	}
	auto &constraints = op.bound_constraints;
	auto truncated = [&](const unique_ptr<BoundConstraint> &constraint) {
		if (constraint->type != ConstraintType::FOREIGN_KEY) {
			return false;
		}
		auto &foreign_key = constraint->Cast<BoundForeignKeyConstraint>();
		if (!foreign_key.info.IsDeleteConstraint()) {
			return false;
		}
		auto &referencing = Catalog::GetEntry<TableCatalogEntry>(
		    context,
		    QualifiedName(op.table.ParentCatalog().GetName(), foreign_key.info.schema, foreign_key.info.table));
		return std::find(op.truncate_group.begin(), op.truncate_group.end(), referencing.oid) !=
		       op.truncate_group.end();
	};
	constraints.erase(std::remove_if(constraints.begin(), constraints.end(), truncated), constraints.end());
}

PhysicalOperator &DuckCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                          PhysicalOperator &plan) {
	DropTruncatedForeignKeys(context, op);
	// Get the row_id column index.
	auto &bound_ref = op.expressions[0]->Cast<BoundReferenceExpression>();
	auto &storage_table = op.table.GetStorageTableEntry(context);
	auto &del = planner.Make<PhysicalDelete>(op.types, storage_table, storage_table.GetStorage(),
	                                         std::move(op.bound_constraints), bound_ref.Index(),
	                                         op.estimated_cardinality, op.return_chunk, std::move(op.return_columns));
	del.children.push_back(plan);
	return del;
}

PhysicalOperator &Catalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op) {
	auto &plan = planner.CreatePlan(*op.children[0]);
	return PlanDelete(context, planner, op, plan);
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalDelete &op) {
	D_ASSERT(op.children.size() == 1);

	dependencies.AddDependency(op.table);
	return op.table.catalog.PlanDelete(context, *this, op);
}

} // namespace duckdb
