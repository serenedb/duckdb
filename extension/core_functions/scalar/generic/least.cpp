#include "core_functions/scalar/generic_functions.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/smaller_binary.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
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

template <class OP>
struct DenseCompare {
	template <class T>
	static inline bool Operation(T left, T right) {
		if constexpr (std::is_floating_point_v<T>) {
			if constexpr (std::is_same_v<OP, GreaterThan>) {
				return ((left > right) | (left != left)) & (right == right);
			} else {
				return ((left < right) | (right != right)) & (left == left);
			}
		} else {
			return OP::Operation(left, right);
		}
	}
};

template <class OP>
struct DenseLeastGreatestOperator {
	template <class T>
	static inline T Operation(T candidate, T current) {
		return DenseCompare<OP>::Operation(candidate, current) ? candidate : current;
	}
};

template <class T, class OP, bool LEFT_CONSTANT, bool RIGHT_CONSTANT>
void DenseLeastGreatestPair(const T *__restrict left, const T *__restrict right, T *__restrict result, idx_t count) {
	for (idx_t i = 0; i < count; i++) {
		result[i] =
		    DenseLeastGreatestOperator<OP>::Operation(right[RIGHT_CONSTANT ? 0 : i], left[LEFT_CONSTANT ? 0 : i]);
	}
}

template <class T, class OP, bool CONSTANT>
void DenseLeastGreatestFold(const T *__restrict input, T *__restrict result, idx_t count) {
	for (idx_t i = 0; i < count; i++) {
		result[i] = DenseLeastGreatestOperator<OP>::Operation(input[CONSTANT ? 0 : i], result[i]);
	}
}

template <class T>
const T *DenseInputData(const Vector &input) {
	return input.GetVectorType() == VectorType::CONSTANT_VECTOR ? ConstantVector::GetData<T>(input)
	                                                            : FlatVector::GetData<T>(input);
}

template <class T, class OP>
bool TryDenseLeastGreatest(DataChunk &args, Vector &result) {
	const auto count = args.size();
	bool has_flat = false;
	idx_t input_count = 0;
	for (auto &input : args.data) {
		if (input.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			if (!ConstantVector::IsNull(input)) {
				input_count++;
			}
			continue;
		}
		if (input.GetVectorType() != VectorType::FLAT_VECTOR || !FlatVector::Validity(input).CheckAllValid(count)) {
			return false;
		}
		has_flat = true;
		input_count++;
	}
	if (!has_flat || input_count < 2) {
		return false;
	}

	result.SetVectorType(VectorType::FLAT_VECTOR);
	if (result.size() != count) {
		FlatVector::SetSize(result, count);
	}
	FlatVector::ValidityMutable(result).Reset(count);
	auto result_data = FlatVector::GetDataMutable<T>(result);
	optional_ptr<const Vector> first;
	bool folded = false;
	for (auto &input : args.data) {
		const auto constant = input.GetVectorType() == VectorType::CONSTANT_VECTOR;
		if (constant && ConstantVector::IsNull(input)) {
			continue;
		}
		if (!first) {
			first = input;
			continue;
		}
		auto input_data = DenseInputData<T>(input);
		if (folded) {
			if (constant) {
				DenseLeastGreatestFold<T, OP, true>(input_data, result_data, count);
			} else {
				DenseLeastGreatestFold<T, OP, false>(input_data, result_data, count);
			}
			continue;
		}
		auto first_data = DenseInputData<T>(*first);
		const auto first_constant = first->GetVectorType() == VectorType::CONSTANT_VECTOR;
		if (first_constant && constant) {
			DenseLeastGreatestPair<T, OP, true, true>(first_data, input_data, result_data, count);
		} else if (first_constant) {
			DenseLeastGreatestPair<T, OP, true, false>(first_data, input_data, result_data, count);
		} else if (constant) {
			DenseLeastGreatestPair<T, OP, false, true>(first_data, input_data, result_data, count);
		} else {
			DenseLeastGreatestPair<T, OP, false, false>(first_data, input_data, result_data, count);
		}
		folded = true;
	}
	return true;
}

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

template <class T, class OP, class BASE_OP = StandardLeastGreatest<false>>
void LeastGreatestFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	if (args.ColumnCount() == 1) {
		// single input: nop
		result.Reference(args.data[0]);
		return;
	}
	if constexpr (std::is_same_v<BASE_OP, StandardLeastGreatest<false>>) {
		if (TryDenseLeastGreatest<T, OP>(args, result)) {
			return;
		}
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
	using OP = typename LEAST_GREATER_OP::OP;
	auto physical_type = child_type.id() == LogicalTypeId::TIME_TZ ? PhysicalType::INVALID : child_type.InternalType();
	switch (physical_type) {
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
	case PhysicalType::UINT8:
		bound_function.SetFunctionCallback(LeastGreatestFunction<uint8_t, OP>);
		break;
	case PhysicalType::UINT16:
		bound_function.SetFunctionCallback(LeastGreatestFunction<uint16_t, OP>);
		break;
	case PhysicalType::UINT32:
		bound_function.SetFunctionCallback(LeastGreatestFunction<uint32_t, OP>);
		break;
	case PhysicalType::UINT64:
		bound_function.SetFunctionCallback(LeastGreatestFunction<uint64_t, OP>);
		break;
	case PhysicalType::UINT128:
		bound_function.SetFunctionCallback(LeastGreatestFunction<uhugeint_t, OP>);
		break;
	case PhysicalType::FLOAT:
		bound_function.SetFunctionCallback(LeastGreatestFunction<float, OP>);
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
