#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/execution/adaptive_filter.hpp"

#include <random>

namespace duckdb {

struct ConjunctionState : public ExpressionState {
	ConjunctionState(const Expression &expr, ExpressionExecutorState &root) : ExpressionState(expr, root) {
		if (HasContext()) {
			adaptive_filter = make_uniq<AdaptiveFilter>(expr, Settings::Get<FilterReorderSetting>(GetContext()));
			adaptive_filter->SetLogger(GetContext().logger);
		} else {
			adaptive_filter = make_uniq<AdaptiveFilter>(
			    expr, EnumUtil::FromString<FilterReorder>(FilterReorderSetting::DefaultValue));
		}
	}
	SelectionVector &TempTrue() {
		if (!temp_true.data()) {
			temp_true.Initialize(STANDARD_VECTOR_SIZE);
		}
		return temp_true;
	}

	SelectionVector &TempFalse() {
		if (!temp_false.data()) {
			temp_false.Initialize(STANDARD_VECTOR_SIZE);
		}
		return temp_false;
	}

	unique_ptr<AdaptiveFilter> adaptive_filter;

private:
	SelectionVector temp_true;
	SelectionVector temp_false;
};

unique_ptr<ExpressionState> ExpressionExecutor::InitializeState(const BoundConjunctionExpression &expr,
                                                                ExpressionExecutorState &root) {
	auto result = make_uniq<ConjunctionState>(expr, root);
	for (auto &child : expr.GetChildren()) {
		result->AddChild(*child);
	}
	if (expr.GetChildren().size() > 1) {
		for (idx_t i = 0; i < 2; i++) {
			result->types.push_back(LogicalType::BOOLEAN);
			result->initialize.push_back(true);
		}
	}

	result->Finalize();
	return std::move(result);
}

void ExpressionExecutor::Execute(const BoundConjunctionExpression &expr, ExpressionState *state,
                                 const SelectionVector *sel, idx_t count, Vector &result) {
	// execute the children
	state->intermediate_chunk.Reset();
	const auto child_count = expr.GetChildren().size();
	for (idx_t i = 0; i < child_count; i++) {
		auto &current_result = state->intermediate_chunk.data[i];
		Execute(*expr.GetChildren()[i], state->child_states[i].get(), sel, count, current_result);
		if (i == 0) {
			// move the result
			result.Reference(current_result);
		} else {
			auto &intermediate = state->intermediate_chunk.data[child_count + i % 2];
			// AND/OR together
			switch (expr.GetExpressionType()) {
			case ExpressionType::CONJUNCTION_AND:
				VectorOperations::And(current_result, result, intermediate);
				break;
			case ExpressionType::CONJUNCTION_OR:
				VectorOperations::Or(current_result, result, intermediate);
				break;
			default:
				throw InternalException("Unknown conjunction type!");
			}
			result.Reference(intermediate);
		}
	}
}

idx_t ExpressionExecutor::Select(const BoundConjunctionExpression &expr, ExpressionState *state_p,
                                 const SelectionVector *sel, idx_t count, SelectionVector *true_sel,
                                 SelectionVector *false_sel) {
	auto &state = state_p->Cast<ConjunctionState>();

	if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		// get runtime statistics
		auto filter_state = state.adaptive_filter->BeginFilter();
		const auto &permutation = state.adaptive_filter->GetPermutation();
		const SelectionVector *current_sel = sel;
		idx_t current_count = count;
		idx_t false_count = 0;

		optional_ptr<SelectionVector> temp_false;
		if (false_sel) {
			temp_false = &state.TempFalse();
		}
		if (!true_sel) {
			true_sel = &state.TempTrue();
		}
		for (idx_t i = 0; i < expr.GetChildren().size(); i++) {
			idx_t tcount = Select(*expr.GetChildren()[permutation[i]], state.child_states[permutation[i]].get(),
			                      current_sel, current_count, true_sel, temp_false.get());
			idx_t fcount = current_count - tcount;
			if (fcount > 0 && false_sel) {
				// move failing tuples into the false_sel
				// tuples passed, move them into the actual result vector
				for (idx_t i = 0; i < fcount; i++) {
					false_sel->set_index(false_count++, temp_false->get_index(i));
				}
			}
			current_count = tcount;
			if (current_count == 0) {
				break;
			}
			if (current_count < count) {
				// tuples were filtered out: move on to using the true_sel to only evaluate passing tuples in subsequent
				// iterations
				current_sel = true_sel;
			}
		}
		// adapt runtime statistics
		state.adaptive_filter->EndFilter(filter_state);
		return current_count;
	} else {
		// get runtime statistics
		auto filter_state = state.adaptive_filter->BeginFilter();
		const auto &permutation = state.adaptive_filter->GetPermutation();

		const SelectionVector *current_sel = sel;
		idx_t current_count = count;
		idx_t result_count = 0;

		optional_ptr<SelectionVector> temp_true;
		if (true_sel) {
			temp_true = &state.TempTrue();
		}
		if (!false_sel) {
			false_sel = &state.TempFalse();
		}
		for (idx_t i = 0; i < expr.GetChildren().size(); i++) {
			idx_t tcount = Select(*expr.GetChildren()[permutation[i]], state.child_states[permutation[i]].get(),
			                      current_sel, current_count, temp_true.get(), false_sel);
			if (tcount > 0) {
				if (true_sel) {
					// tuples passed, move them into the actual result vector
					for (idx_t i = 0; i < tcount; i++) {
						true_sel->set_index(result_count++, temp_true->get_index(i));
					}
				}
				// now move on to check only the non-passing tuples
				current_count -= tcount;
				current_sel = false_sel;
			}
		}
		if (true_sel) {
			true_sel->Sort(result_count);
		}

		// adapt runtime statistics
		state.adaptive_filter->EndFilter(filter_state);
		return result_count;
	}
}

} // namespace duckdb
