#include "core_functions/scalar/generic_functions.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/smaller_binary.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/collation_binding.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

namespace duckdb {

namespace {
struct LeastOp {
	using OP = LessThan;

	static OrderByNullType NullOrdering() {
		return OrderByNullType::NULLS_LAST;
	}
};

struct GreaterOp {
	using OP = GreaterThan;

	static OrderByNullType NullOrdering() {
		return OrderByNullType::NULLS_FIRST;
	}
};

template <class OP>
struct LeastOperator {
	template <class T>
	static T Operation(T left, T right) {
		return OP::Operation(left, right) ? left : right;
	}
};

struct LeastGreatestSortKeyState : public FunctionLocalState {
	explicit LeastGreatestSortKeyState(idx_t column_count, OrderByNullType null_ordering)
	    : intermediate(LogicalType::BLOB), modifiers(OrderType::ASCENDING, null_ordering) {
		vector<LogicalType> types;
		// initialize sort key chunk
		for (idx_t i = 0; i < column_count; i++) {
			types.push_back(LogicalType::BLOB);
		}
		sort_keys.Initialize(Allocator::DefaultAllocator(), types);
	}

	DataChunk sort_keys;
	Vector intermediate;
	OrderModifiers modifiers;
};

template <class OP>
unique_ptr<FunctionLocalState> LeastGreatestSortKeyInit(ExpressionState &state, const BoundFunctionExpression &expr,
                                                        FunctionData *bind_data) {
	return make_uniq<LeastGreatestSortKeyState>(expr.GetChildren().size(), OP::NullOrdering());
}

struct LeastGreatestCollatedState : public LeastGreatestSortKeyState {
	LeastGreatestCollatedState(idx_t column_count, OrderByNullType null_ordering, const LogicalType &pair_type)
	    : LeastGreatestSortKeyState(column_count, null_ordering), pairs(pair_type) {
	}

	Vector pairs;
};

template <class OP>
unique_ptr<FunctionLocalState> LeastGreatestCollatedInit(ExpressionState &state, const BoundFunctionExpression &expr,
                                                         FunctionData *bind_data) {
	return make_uniq<LeastGreatestCollatedState>(expr.GetChildren().size(), OP::NullOrdering(),
	                                             expr.GetChildren()[0]->GetReturnType());
}

template <bool STRING>
struct StandardLeastGreatest {
	static constexpr bool IS_STRING = STRING;

	static DataChunk &Prepare(DataChunk &args, ExpressionState &) {
		return args;
	}

	static Vector &TargetVector(Vector &result, ExpressionState &) {
		return result;
	}

	static void FinalizeResult(idx_t rows, bool result_has_value[], Vector &result, ExpressionState &) {
		auto &result_mask = FlatVector::ValidityMutable(result);
		for (idx_t i = 0; i < rows; i++) {
			if (!result_has_value[i]) {
				result_mask.SetInvalid(i);
			}
		}
	}
};

struct SortKeyLeastGreatest {
	static constexpr bool IS_STRING = false;

	static DataChunk &Prepare(DataChunk &args, ExpressionState &state) {
		auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<LeastGreatestSortKeyState>();
		lstate.sort_keys.Reset();
		for (idx_t c_idx = 0; c_idx < args.ColumnCount(); c_idx++) {
			CreateSortKeyHelpers::CreateSortKey(args.data[c_idx], lstate.modifiers, lstate.sort_keys.data[c_idx]);
		}
		return lstate.sort_keys;
	}

	static Vector &TargetVector(Vector &result, ExpressionState &state) {
		auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<LeastGreatestSortKeyState>();
		return lstate.intermediate;
	}

	static void FinalizeResult(idx_t rows, bool result_has_value[], Vector &result, ExpressionState &state) {
		auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<LeastGreatestSortKeyState>();
		auto result_keys = FlatVector::GetData<string_t>(lstate.intermediate);
		auto &result_mask = FlatVector::ValidityMutable(result);
		for (idx_t i = 0; i < rows; i++) {
			if (!result_has_value[i]) {
				result_mask.SetInvalid(i);
			} else {
				CreateSortKeyHelpers::DecodeSortKey(result_keys[i], result, i, lstate.modifiers);
			}
		}
	}
};

struct CollatedSortKeyLeastGreatest : public SortKeyLeastGreatest {
	static void FinalizeResult(idx_t rows, bool result_has_value[], Vector &result, ExpressionState &state) {
		auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<LeastGreatestCollatedState>();
		auto result_keys = FlatVector::GetData<string_t>(lstate.intermediate);
		auto &values = StructVector::GetEntries(lstate.pairs)[1];
		auto &result_mask = FlatVector::ValidityMutable(result);
		for (idx_t i = 0; i < rows; i++) {
			if (!result_has_value[i]) {
				result_mask.SetInvalid(i);
				continue;
			}
			CreateSortKeyHelpers::DecodeSortKey(result_keys[i], lstate.pairs, i, lstate.modifiers);
			VectorOperations::Copy(values, result, i + 1, i, i);
		}
	}
};

template <class T, class OP, class BASE_OP = StandardLeastGreatest<false>>
void LeastGreatestFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	if (args.ColumnCount() == 1) {
		// single input: nop
		result.Reference(args.data[0]);
		return;
	}
	auto &input = BASE_OP::Prepare(args, state);
	auto &result_vector = BASE_OP::TargetVector(result, state);

	auto result_type = VectorType::CONSTANT_VECTOR;
	for (idx_t col_idx = 0; col_idx < input.ColumnCount(); col_idx++) {
		if (args.data[col_idx].GetVectorType() != VectorType::CONSTANT_VECTOR) {
			// non-constant input: result is not a constant vector
			result_type = VectorType::FLAT_VECTOR;
		}
		if (BASE_OP::IS_STRING) {
			// for string vectors we add a reference to the heap of the children
			StringVector::AddHeapReference(result_vector, input.data[col_idx]);
		}
	}

	auto result_data = FlatVector::ScatterWriter<T>(result_vector);
	bool result_has_value[STANDARD_VECTOR_SIZE] {false};
	// perform the operation column-by-column
	for (idx_t col_idx = 0; col_idx < input.ColumnCount(); col_idx++) {
		if (input.data[col_idx].GetVectorType() == VectorType::CONSTANT_VECTOR &&
		    ConstantVector::IsNull(input.data[col_idx])) {
			// ignore null vector
			continue;
		}

		auto entries = input.data[col_idx].template Values<T>();

		if (entries.CanHaveNull()) {
			// potential new null entries: have to check the null mask
			for (idx_t i = 0; i < input.size(); i++) {
				auto entry = entries[i];
				if (entry.IsValid()) {
					// not a null entry: perform the operation and add to new set
					auto ivalue = entry.GetValue();
					if (!result_has_value[i] || OP::template Operation<T>(ivalue, result_data[i])) {
						result_has_value[i] = true;
						result_data[i] = ivalue;
					}
				}
			}
		} else {
			// no new null entries: only need to perform the operation
			for (idx_t i = 0; i < input.size(); i++) {
				auto ivalue = entries.GetValueUnsafe(i);
				if (!result_has_value[i] || OP::template Operation<T>(ivalue, result_data[i])) {
					result_has_value[i] = true;
					result_data[i] = ivalue;
				}
			}
		}
	}
	BASE_OP::FinalizeResult(input.size(), result_has_value, result, state);
	result.SetVectorType(result_type);
}

template <class LEAST_GREATER_OP>
unique_ptr<BaseStatistics> PropagateLeastGreatestStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	auto &return_type = expr.GetReturnType();
	if (BaseStatistics::GetStatsType(return_type) != StatisticsType::NUMERIC_STATS ||
	    return_type.InternalType() == PhysicalType::BOOL) {
		return nullptr;
	}
	constexpr bool IS_LEAST = std::is_same_v<LEAST_GREATER_OP, LeastOp>;

	auto merge = [](Value &bound, const Value &candidate, bool keep_smaller) {
		if (bound.IsNull() || (keep_smaller ? candidate < bound : candidate > bound)) {
			bound = candidate;
		}
	};

	// least/greatest always returns one of its inputs, so the result range is built from the
	// inputs' min/max values. A NULL input is skipped at runtime, so only inputs that can never
	// be NULL may tighten the "anchored" side of the range:
	//
	//             | loose side (over all inputs) | anchored side (over non-null inputs)
	//   least     | min of the mins  -> min      | min of the maxes -> max
	//   greatest  | max of the maxes -> max      | max of the mins  -> min
	//
	// If every input can be NULL, a single input may end up deciding the result, so the anchored
	// side falls back to the opposite aggregation over all inputs
	Value loose;
	Value anchored;          // over non-null inputs only
	Value anchored_fallback; // over all inputs; used only when every input is nullable
	bool has_nonnull_input = false;
	for (auto &cs : child_stats) {
		if (cs.GetStatsType() != StatisticsType::NUMERIC_STATS || !NumericStats::HasMinMax(cs)) {
			return nullptr;
		}
		const Value cmin = NumericStats::Min(cs);
		const Value cmax = NumericStats::Max(cs);
		const Value &loose_val = IS_LEAST ? cmin : cmax;
		const Value &anchored_val = IS_LEAST ? cmax : cmin;

		merge(loose, loose_val, /*keep_smaller=*/IS_LEAST);
		merge(anchored_fallback, anchored_val, /*keep_smaller=*/!IS_LEAST);
		if (!cs.CanHaveNull()) {
			has_nonnull_input = true;
			merge(anchored, anchored_val, /*keep_smaller=*/IS_LEAST);
		}
	}
	if (!has_nonnull_input) {
		anchored = std::move(anchored_fallback);
	}

	auto result = NumericStats::CreateEmpty(return_type);
	if constexpr (IS_LEAST) {
		NumericStats::SetMin(result, loose);
		NumericStats::SetMax(result, anchored);
	} else {
		NumericStats::SetMin(result, anchored);
		NumericStats::SetMax(result, loose);
	}
	result.Set(StatsInfo::CAN_HAVE_VALID_VALUES);
	if (!has_nonnull_input) {
		// the result is NULL only if all inputs are NULL
		result.Set(StatsInfo::CAN_HAVE_NULL_VALUES);
	}
	return result.ToUnique();
}

template <class LEAST_GREATER_OP>
bool TryBindCollatedLeastGreatest(ClientContext &context, BoundScalarFunction &bound_function,
                                  vector<unique_ptr<Expression>> &arguments, const LogicalType &child_type) {
	if (arguments.size() < 2 || !CollationBinding::Get(context).HasTypeCollation(context, child_type)) {
		return false;
	}
	const Identifier parameter("x");
	const BoundReferenceExpression parameter_ref(parameter, child_type, idx_t(0));
	unique_ptr<Expression> key = parameter_ref.Copy();
	if (!ExpressionBinder::PushCollation(context, key, child_type)) {
		return false;
	}
	key->SetAlias(Identifier("key"));
	auto value = parameter_ref.Copy();
	value->SetAlias(Identifier("value"));
	FunctionBinder function_binder(context);
	vector<unique_ptr<Expression>> fields;
	fields.push_back(std::move(key));
	fields.push_back(std::move(value));
	auto pair =
	    function_binder.BindScalarFunction(Identifier::DefaultSchema(), Identifier("struct_pack"), std::move(fields));
	auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
	is_null->GetChildrenMutable().push_back(parameter_ref.Copy());
	auto null_pair = make_uniq<BoundConstantExpression>(Value(pair->GetReturnType()));
	const auto body = make_uniq<BoundCaseExpression>(std::move(is_null), std::move(null_pair), std::move(pair));

	vector<unique_ptr<Expression>> pairs;
	for (auto &argument : arguments) {
		auto lambda =
		    make_uniq<BoundLambdaExpression>(ExpressionType::LAMBDA, LogicalType::LAMBDA, body->Copy(), idx_t(1));
		lambda->SetParameterNames({parameter});
		vector<unique_ptr<Expression>> invoke_arguments;
		invoke_arguments.push_back(std::move(lambda));
		invoke_arguments.push_back(BoundCastExpression::AddCastToType(context, std::move(argument), child_type));
		pairs.push_back(
		    function_binder.BindScalarFunction(Identifier::DefaultSchema(), "invoke", std::move(invoke_arguments)));
	}
	const auto pair_type = pairs[0]->GetReturnType();
	arguments = std::move(pairs);
	for (auto &arg : bound_function.GetArguments()) {
		arg = pair_type;
	}
	bound_function.SetReturnType(child_type);
	bound_function.SetFunctionCallback(
	    LeastGreatestFunction<string_t, typename LEAST_GREATER_OP::OP, CollatedSortKeyLeastGreatest>);
	bound_function.SetInitStateCallback(LeastGreatestCollatedInit<LEAST_GREATER_OP>);
	return true;
}

template <class LEAST_GREATER_OP>
unique_ptr<FunctionData> BindLeastGreatest(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	LogicalType child_type = ExpressionBinder::GetExpressionReturnType(*arguments[0]);
	for (idx_t i = 1; i < arguments.size(); i++) {
		auto arg_type = ExpressionBinder::GetExpressionReturnType(*arguments[i]);
		if (!LogicalType::TryGetMaxLogicalType(context, child_type, arg_type, child_type)) {
			throw BinderException(arguments[i]->GetQueryLocation(),
			                      "Cannot combine types of %s and %s - an explicit cast is required",
			                      child_type.ToString(), arg_type.ToString());
		}
	}
	switch (child_type.id()) {
	case LogicalTypeId::UNKNOWN:
		throw ParameterNotResolvedException();
	case LogicalTypeId::INTEGER_LITERAL:
		child_type = IntegerLiteral::GetType(child_type);
		break;
	case LogicalTypeId::STRING_LITERAL:
		child_type = LogicalType::VARCHAR;
		break;
	default:
		break;
	}
	if (TryBindCollatedLeastGreatest<LEAST_GREATER_OP>(context, bound_function, arguments, child_type)) {
		return nullptr;
	}
	using OP = typename LEAST_GREATER_OP::OP;
	switch (child_type.InternalType()) {
#if !DUCKDB_SMALLER_BINARY(least_greatest_types)
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		bound_function.SetFunctionCallback(LeastGreatestFunction<int8_t, OP>);
		break;
	case PhysicalType::INT16:
		bound_function.SetFunctionCallback(LeastGreatestFunction<int16_t, OP>);
		break;
	case PhysicalType::INT32:
		bound_function.SetFunctionCallback(LeastGreatestFunction<int32_t, OP>);
		break;
	case PhysicalType::INT64:
		bound_function.SetFunctionCallback(LeastGreatestFunction<int64_t, OP>);
		break;
	case PhysicalType::INT128:
		bound_function.SetFunctionCallback(LeastGreatestFunction<hugeint_t, OP>);
		break;
	case PhysicalType::DOUBLE:
		bound_function.SetFunctionCallback(LeastGreatestFunction<double, OP>);
		break;
	case PhysicalType::VARCHAR:
		bound_function.SetFunctionCallback(LeastGreatestFunction<string_t, OP, StandardLeastGreatest<true>>);
		break;
#endif
	default:
		// fallback with sort keys
		bound_function.SetFunctionCallback(LeastGreatestFunction<string_t, OP, SortKeyLeastGreatest>);
		bound_function.SetInitStateCallback(LeastGreatestSortKeyInit<LEAST_GREATER_OP>);
		break;
	}
	for (auto &arg : bound_function.GetArguments()) {
		arg = child_type;
	}
	bound_function.SetReturnType(child_type);
	return nullptr;
}

template <class OP>
ScalarFunction GetLeastGreatestFunction() {
	ScalarFunction fun({{"arg1", LogicalType::ANY}}, LogicalType::ANY, nullptr, BindLeastGreatest<OP>,
	                   PropagateLeastGreatestStats<OP>, nullptr, LogicalType::ANY, FunctionStability::CONSISTENT,
	                   FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

template <class OP>
ScalarFunctionSet GetLeastGreatestFunctions() {
	ScalarFunctionSet fun_set;
	fun_set.AddFunction(GetLeastGreatestFunction<OP>());
	return fun_set;
}

} // namespace

ScalarFunctionSet LeastFun::GetFunctions() {
	return GetLeastGreatestFunctions<LeastOp>();
}

ScalarFunctionSet GreatestFun::GetFunctions() {
	return GetLeastGreatestFunctions<GreaterOp>();
}

} // namespace duckdb
