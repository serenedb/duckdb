#include "duckdb/common/tree_renderer.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/operator/helper/physical_explain_analyze.hpp"
#include "duckdb/execution/operator/scan/physical_column_data_scan.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_explain.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalExplain &op) {
	D_ASSERT(op.children.size() == 1);
	auto explain_output = Settings::Get<ExplainOutputSetting>(context);
	// The optimized logical plan can only be rendered before CreatePlan moves expressions out of
	// the logical tree - but only render it when the current mode outputs it.
	string logical_plan_opt;
	if (op.explain_type != ExplainType::EXPLAIN_ANALYZE && explain_output != ExplainOutputType::PHYSICAL_ONLY) {
		logical_plan_opt = op.children[0]->ToString(context, op.format);
	}
	auto &plan = CreatePlan(*op.children[0]);
	if (op.explain_type == ExplainType::EXPLAIN_ANALYZE) {
		auto &explain = Make<PhysicalExplainAnalyze>(op.types, op.format);
		explain.children.push_back(plan);
		return explain;
	}

	// Format the plan and set the output of the EXPLAIN.
	vector<string> keys, values;
	switch (explain_output) {
	case ExplainOutputType::OPTIMIZED_ONLY:
		keys = {"logical_opt"};
		values = {std::move(logical_plan_opt)};
		break;
	case ExplainOutputType::PHYSICAL_ONLY:
		op.physical_plan = plan.ToString(context, op.format);
		keys = {"physical_plan"};
		values = {op.physical_plan};
		break;
	default:
		op.physical_plan = plan.ToString(context, op.format);
		keys = {"logical_plan", "logical_opt", "physical_plan"};
		values = {op.logical_plan_unopt, std::move(logical_plan_opt), op.physical_plan};
	}

	// Pack the plan strings into the result columns. PG shape: a single "QUERY PLAN"
	// column with one row per plan line; DuckDB native shape: two columns {key, value}
	// with one row per plan section.
	auto &allocator = Allocator::Get(context);
	auto collection = make_uniq<ColumnDataCollection>(context, op.types, ColumnDataAllocatorType::IN_MEMORY_ALLOCATOR);

	DataChunk chunk;
	chunk.Initialize(allocator, op.types);
	if (op.output_shape == ExplainFormatShape::PG) {
		for (idx_t i = 0; i < values.size(); i++) {
			AppendExplainLines(values[i], chunk, *collection);
		}
	} else {
		for (idx_t i = 0; i < keys.size(); i++) {
			chunk.SetValue(0, chunk.size(), Value(keys[i]));
			chunk.SetValue(1, chunk.size(), Value(values[i]));
			chunk.SetCardinality(chunk.size() + 1);
			if (chunk.size() == STANDARD_VECTOR_SIZE) {
				collection->Append(chunk);
				chunk.Reset();
			}
		}
	}
	collection->Append(chunk);

	// Output the result via a chunk scan.
	return Make<PhysicalColumnDataScan>(op.types, PhysicalOperatorType::COLUMN_DATA_SCAN, op.estimated_cardinality,
	                                    std::move(collection));
}

} // namespace duckdb
