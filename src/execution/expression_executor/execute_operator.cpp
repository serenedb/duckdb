#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/string_map_set.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"

#include <absl/container/flat_hash_set.h>

namespace duckdb {

namespace {

struct CoalesceState : public ExpressionState {
	CoalesceState(const Expression &expr, ExpressionExecutorState &root) : ExpressionState(expr, root) {
	}

	void Reserve(idx_t count) {
		if (count <= capacity) {
			return;
		}
		capacity = MaxValue<idx_t>(count, STANDARD_VECTOR_SIZE);
		sel_a.Initialize(capacity);
		sel_b.Initialize(capacity);
		slice_sel.Initialize(capacity);
		result_sel.Initialize(capacity);
	}

	idx_t capacity = 0;
	SelectionVector sel_a;
	SelectionVector sel_b;
	SelectionVector slice_sel;
	SelectionVector result_sel;
};

constexpr idx_t IN_SET_THRESHOLD = 16;

bool InSetSupportsType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::UUID:
	case LogicalTypeId::ENUM:
		return true;
	default:
		return false;
	}
}

struct InState : public ExpressionState {
	InState(const Expression &expr, ExpressionExecutorState &root) : ExpressionState(expr, root) {
	}

	void Initialize(const BoundOperatorExpression &expr) {
		initialized = true;
		auto &children = expr.GetChildren();
		auto &type = children[0]->GetReturnType();
		if (children.size() - 1 < IN_SET_THRESHOLD || !InSetSupportsType(type) || !HasContext()) {
			return;
		}
		for (idx_t i = 1; i < children.size(); i++) {
			if (!children[i]->IsFoldable() || children[i]->GetReturnType() != type) {
				return;
			}
		}
		Vector values(type, children.size() - 1);
		for (idx_t i = 1; i < children.size(); i++) {
			auto value = ExpressionExecutor::EvaluateScalar(GetContext(), *children[i]);
			list_has_null = list_has_null || value.IsNull();
			values.Append(value, VectorAppendMode::ALLOW_RESIZE);
		}
		keys = make_uniq<Vector>(LogicalType::BLOB, values.size());
		MapVector::CreateKeySortKeys(values, *keys);
		auto list_keys = keys->Values<string_t>();
		auto validity = values.Validity();
		for (idx_t i = 0; i < values.size(); i++) {
			if (validity.IsValid(i)) {
				set.insert(list_keys[i].GetValue());
			}
		}
		use_set = true;
	}

	bool initialized = false;
	bool use_set = false;
	bool list_has_null = false;
	unique_ptr<Vector> keys;
	absl::flat_hash_set<string_t, StringHash, StringEquality> set;
};

} // namespace

unique_ptr<ExpressionState> ExpressionExecutor::InitializeState(const BoundOperatorExpression &expr,
                                                                ExpressionExecutorState &root) {
	unique_ptr<ExpressionState> result;
	const auto expression_type = expr.GetExpressionType();
	const bool is_in =
	    expression_type == ExpressionType::COMPARE_IN || expression_type == ExpressionType::COMPARE_NOT_IN;
	if (expression_type == ExpressionType::OPERATOR_COALESCE) {
		result = make_uniq<CoalesceState>(expr, root);
	} else if (is_in) {
		result = make_uniq<InState>(expr, root);
	} else {
		result = make_uniq<ExpressionState>(expr, root);
	}
	for (auto &child : expr.GetChildren()) {
		result->AddChild(*child);
	}
	if (is_in) {
		for (idx_t i = 0; i < 3; i++) {
			result->types.push_back(LogicalType::BOOLEAN);
			result->initialize.push_back(true);
		}
	}

	result->Finalize();
	return result;
}

void ExpressionExecutor::Execute(const BoundOperatorExpression &expr, ExpressionState *state,
                                 const SelectionVector *sel, idx_t count, Vector &result) {
	// special handling for special snowflake 'IN'
	// IN has n children
	auto expression_type = expr.GetExpressionType();
	if (expression_type == ExpressionType::COMPARE_IN || expression_type == ExpressionType::COMPARE_NOT_IN) {
		auto &children = expr.GetChildren();
		if (children.size() < 2) {
			throw InvalidInputException("IN needs at least two children");
		}
		auto &in_state = state->Cast<InState>();
		if (!in_state.initialized) {
			in_state.Initialize(expr);
		}
		auto &chunk = state->intermediate_chunk;
		chunk.Reset();
		auto &left = chunk.data[0];
		// eval left side
		Execute(*children[0], state->child_states[0].get(), sel, count, left);

		if (in_state.use_set && left.GetVectorType() != VectorType::CONSTANT_VECTOR) {
			Vector left_keys(LogicalType::BLOB, count);
			MapVector::CreateKeySortKeys(left, left_keys);
			auto keys = left_keys.Values<string_t>();
			auto validity = left.Validity();
			const bool in = expression_type == ExpressionType::COMPARE_IN;
			auto writer = FlatVector::Writer<bool>(result, count);
			for (idx_t i = 0; i < count; i++) {
				if (!validity.IsValid(i)) {
					writer.WriteNull();
				} else if (in_state.set.contains(keys[i].GetValue())) {
					writer.WriteValue(in);
				} else if (in_state.list_has_null) {
					writer.WriteNull();
				} else {
					writer.WriteValue(!in);
				}
			}
			return;
		}

		auto &comp_res = chunk.data[children.size()];
		Vector intermediate(LogicalType::BOOLEAN, 0);

		// in rhs is a list of constants
		// for every child, OR the result of the comparison with the left
		// to get the overall result.
		for (idx_t child = 1; child < children.size(); child++) {
			auto &vector_to_check = chunk.data[child];

			Execute(*children[child], state->child_states[child].get(), sel, count, vector_to_check);
			auto &accumulator = chunk.data[children.size() + 1 + child % 2];
			if (child == 1) {
				// first child: move to result
				VectorOperations::Equals(left, vector_to_check, accumulator);
			} else {
				// otherwise OR together
				VectorOperations::Equals(left, vector_to_check, comp_res);
				VectorOperations::Or(intermediate, comp_res, accumulator);
			}
			intermediate.Reference(accumulator);
		}
		if (expression_type == ExpressionType::COMPARE_NOT_IN) {
			// NOT IN: invert result
			VectorOperations::Not(intermediate, result);
		} else {
			// directly use the result
			result.Reference(intermediate);
		}
	} else if (expression_type == ExpressionType::OPERATOR_COALESCE) {
		auto &coalesce_state = state->Cast<CoalesceState>();
		coalesce_state.Reserve(count);
		auto &sel_a = coalesce_state.sel_a;
		auto &sel_b = coalesce_state.sel_b;
		auto &slice_sel = coalesce_state.slice_sel;
		auto &result_sel = coalesce_state.result_sel;
		SelectionVector *next_sel = &sel_a;
		const SelectionVector *current_sel = sel;
		idx_t remaining_count = count;
		idx_t next_count;
		state->intermediate_chunk.Reset();
		for (idx_t child = 0; child < expr.GetChildren().size(); child++) {
			auto &vector_to_check = state->intermediate_chunk.data[child];
			Execute(*expr.GetChildren()[child], state->child_states[child].get(), current_sel, remaining_count,
			        vector_to_check);

			auto entries = vector_to_check.Validity();
			idx_t result_count = 0;
			next_count = 0;
			for (idx_t i = 0; i < remaining_count; i++) {
				auto base_idx = current_sel ? current_sel->get_index(i) : i;
				if (entries.IsValid(i)) {
					slice_sel.set_index(result_count, i);
					result_sel.set_index(result_count++, base_idx);
				} else {
					next_sel->set_index(next_count++, base_idx);
				}
			}
			if (result_count > 0) {
				vector_to_check.Slice(slice_sel, result_count);
				FillSwitch(vector_to_check, result, result_sel, NumericCast<sel_t>(result_count));
			}
			current_sel = next_sel;
			next_sel = next_sel == &sel_a ? &sel_b : &sel_a;
			remaining_count = next_count;
			if (next_count == 0) {
				break;
			}
		}
		if (remaining_count > 0) {
			for (idx_t i = 0; i < remaining_count; i++) {
				FlatVector::SetNull(result, current_sel->get_index(i), true);
			}
		}
		if (sel) {
			result.Slice(*sel, count);
		} else if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	} else if (expression_type == ExpressionType::OPERATOR_TRY) {
		auto &child_state = *state->child_states[0];
		Vector try_result(result.GetType());
		try {
			Execute(*expr.GetChildren()[0], &child_state, sel, count, try_result);
			result.Reference(try_result);
			return;
		} catch (std::exception &ex) {
			ErrorData error(ex);
			auto error_type = error.Type();
			if (!Exception::IsExecutionError(error_type)) {
				throw;
			}
		}

		// On error, evaluate per row
		// CASE/COALESCE write their result at the physical row index, so the intermediate must fit that index
		SelectionVector selvec(1);
		DataChunk intermediate;
		intermediate.Initialize(GetAllocator(), {result.GetType()}, STANDARD_VECTOR_SIZE);
		for (idx_t i = 0; i < count; i++) {
			intermediate.Reset();
			intermediate.SetChildCardinality(1);

			// Make sure to clear any dictionary states in the child expression, so that it actually
			// gets executed anew for every row
			child_state.ResetDictionaryStates();

			selvec.set_index(0, sel ? sel->get_index(i) : i);
			Value val(result.GetType());
			try {
				Execute(*expr.GetChildren()[0], &child_state, &selvec, 1, intermediate.data[0]);
				val = intermediate.GetValue(0, 0);
			} catch (std::exception &ex) {
				ErrorData error(ex);
				auto error_type = error.Type();
				if (!Exception::IsExecutionError(error_type)) {
					throw;
				}
			}
			result.SetValue(i, val);
		}
		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	} else if (expr.GetChildren().size() == 1) {
		state->intermediate_chunk.Reset();
		auto &child = state->intermediate_chunk.data[0];

		Execute(*expr.GetChildren()[0], state->child_states[0].get(), sel, count, child);
		switch (expr.GetExpressionType()) {
		case ExpressionType::OPERATOR_NOT: {
			VectorOperations::Not(child, result);
			break;
		}
		case ExpressionType::OPERATOR_IS_NULL: {
			VectorOperations::IsNull(child, result);
			break;
		}
		case ExpressionType::OPERATOR_IS_NOT_NULL: {
			VectorOperations::IsNotNull(child, result);
			break;
		}
		default:
			throw NotImplementedException("Unsupported operator type with 1 child!");
		}
	} else {
		throw NotImplementedException("operator");
	}
}

} // namespace duckdb
