#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/operator/projection/physical_pivot.hpp"

#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"

namespace duckdb {

PhysicalPivot::PhysicalPivot(PhysicalPlan &physical_plan, vector<LogicalType> types_p, PhysicalOperator &child,
                             BoundPivotInfo bound_pivot_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::PIVOT, std::move(types_p), child.estimated_cardinality),
      bound_pivot(std::move(bound_pivot_p)) {
	children.push_back(child);
	for (idx_t p = 0; p < bound_pivot.pivot_values.size(); p++) {
		auto entry = pivot_map.find(bound_pivot.pivot_values[p]);
		if (entry != pivot_map.end()) {
			continue;
		}
		pivot_map[bound_pivot.pivot_values[p]] = bound_pivot.group_count + p;
	}
	// extract the empty aggregate expressions
	for (auto &aggr_expr : bound_pivot.aggregates) {
		auto &aggr = aggr_expr->Cast<BoundAggregateExpression>();
		// for each aggregate, initialize an empty aggregate state and finalize it immediately
		AggregateStateInput state_input(aggr.Function(), aggr.BindInfo().get());
		auto state = make_unsafe_uniq_array<data_t>(aggr.Function().GetStateSizeCallback()(state_input));
		data_ptr_t state_ptr = state.get();
		aggr.Function().GetStateInitCallback()(state_input, &state_ptr, 1);
		Vector state_vector(Value::POINTER(CastPointerToValue(state.get())), count_t(1));
		Vector result_vector(aggr_expr->GetReturnType());
		AggregateFinalizeInputData aggr_input_data(aggr, physical_plan.ArenaRef());
		aggr.Function().GetStateFinalizeCallback()(state_vector, aggr_input_data, result_vector, 1, 0);
		empty_aggregates.push_back(result_vector.GetValue(0));
	}
}

OperatorResultType PhysicalPivot::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                          GlobalOperatorState &gstate, OperatorState &state) const {
	// copy the groups as-is
	input.Flatten();
	for (idx_t i = 0; i < bound_pivot.group_count; i++) {
		chunk.data[i].Reference(input.data[i]);
	}
	auto pivot_column_lists = FlatVector::GetData<list_entry_t>(input.data.back());
	auto &pivot_column_values = ListVector::GetChild(input.data.back());
	auto pivot_columns = FlatVector::GetData<string_t>(pivot_column_values);

	// initialize all aggregate columns with the empty aggregate value
	// if there are multiple aggregates the columns are in order of [AGGR1][AGGR2][AGGR1][AGGR2]
	// so we need to alternate the empty_aggregate that we use
	idx_t aggregate = 0;
	for (idx_t c = bound_pivot.group_count; c < chunk.ColumnCount(); c++) {
		chunk.data[c].Reference(empty_aggregates[aggregate], count_t(input.size()));
		chunk.data[c].Flatten();
		aggregate++;
		if (aggregate >= empty_aggregates.size()) {
			aggregate = 0;
		}
	}

	// move the pivots to the given columns
	const auto aggregate_count = empty_aggregates.size();
	const auto value_columns = chunk.ColumnCount() - bound_pivot.group_count;
	struct PivotMatch {
		idx_t row;
		idx_t element;
		idx_t column;
	};
	vector<PivotMatch> matches;
	vector<idx_t> column_start(value_columns + 1, 0);
	for (idx_t r = 0; r < input.size(); r++) {
		auto list = pivot_column_lists[r];
		for (idx_t aggr = 0; aggr < aggregate_count; aggr++) {
			auto pivot_value_lists = FlatVector::GetData<list_entry_t>(input.data[bound_pivot.group_count + aggr]);
			if (list.length != pivot_value_lists[r].length) {
				throw InternalException("Pivot - unaligned lists between values and columns!?");
			}
		}
		for (idx_t l = 0; l < list.length; l++) {
			// figure out the column value number of this list
			auto &column_name = pivot_columns[list.offset + l];
			auto entry = pivot_map.find(column_name);
			if (entry == pivot_map.end()) {
				// column entry not found in map - that means this element is explicitly excluded from the pivot list
				continue;
			}
			auto column = entry->second - bound_pivot.group_count;
			matches.push_back({r, l, column});
			for (idx_t aggr = 0; aggr < aggregate_count; aggr++) {
				column_start[column + aggr + 1]++;
			}
		}
	}
	for (idx_t c = 0; c < value_columns; c++) {
		column_start[c + 1] += column_start[c];
	}
	vector<idx_t> next(column_start.begin(), column_start.end() - 1);
	vector<idx_t> target_rows(column_start.back());
	SelectionVector sources(MaxValue<idx_t>(column_start.back(), 1));
	for (auto &match : matches) {
		for (idx_t aggr = 0; aggr < aggregate_count; aggr++) {
			auto pivot_value_lists = FlatVector::GetData<list_entry_t>(input.data[bound_pivot.group_count + aggr]);
			auto position = next[match.column + aggr]++;
			target_rows[position] = match.row;
			sources.set_index(position, pivot_value_lists[match.row].offset + match.element);
		}
	}
	for (idx_t c = 0; c < value_columns; c++) {
		auto &source = ListVector::GetChild(input.data[bound_pivot.group_count + c % aggregate_count]);
		auto &target = chunk.data[bound_pivot.group_count + c];
		for (idx_t begin = column_start[c]; begin < column_start[c + 1];) {
			auto end = begin + 1;
			while (end < column_start[c + 1] && target_rows[end] == target_rows[end - 1] + 1) {
				end++;
			}
			VectorOperations::Copy(source, target, sources, end, begin, target_rows[begin]);
			begin = end;
		}
	}
	return OperatorResultType::NEED_MORE_INPUT;
}

} // namespace duckdb
