#include "core_functions/scalar/date_functions.hpp"
#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/operator/date_trunc_operators.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

namespace {

template <class TA, class TR, class OP>
inline TR TruncateFinite(TA input) {
	if (input.IsFinite()) {
		return OP::template Operation<TA, TR>(input);
	} else {
		return Cast::template Operation<TA, TR>(input);
	}
}

template <class OP>
struct FiniteOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return TruncateFinite<TA, TR, OP>(input);
	}
};

struct DateTruncBinaryOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA specifier, TB date) {
		return DateTrunc::Element<TB, TR>(GetDatePartSpecifier(specifier.GetString()), date);
	}
};

template <class TA, class TR, class OP>
void DateTruncUnaryFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 2);
	UnaryExecutor::Execute<TA, TR, FiniteOperator<OP>>(args.data[1], result);
}

template <typename TA, typename TR>
scalar_function_t DateTruncCallback(DatePartSpecifier type) {
	return DateTrunc::Dispatch(
	    type, [](auto op) -> scalar_function_t { return DateTruncUnaryFunction<TA, TR, decltype(op)>; });
}

template <typename TA, typename TR>
void DateTruncFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 2);
	const auto &part_arg = args.data[0];
	const auto &date_arg = args.data[1];

	if (part_arg.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		// Common case of constant part.
		if (ConstantVector::IsNull(part_arg)) {
			throw InternalException("DateTrunc called with constant NULL part");
		}
		const auto type = GetDatePartSpecifier(ConstantVector::GetData<string_t>(part_arg)->GetString());
		DateTruncCallback<TA, TR>(type)(args, state, result);
	} else {
		BinaryExecutor::ExecuteStandard<string_t, TA, TR, DateTruncBinaryOperator>(part_arg, date_arg, result);
	}
}

template <class TA, class TR, class OP>
unique_ptr<BaseStatistics> DateTruncStatistics(vector<BaseStatistics> &child_stats) {
	// we can only propagate date stats if the child has stats
	auto &nstats = child_stats[1];
	if (!NumericStats::HasMinMax(nstats)) {
		return nullptr;
	}
	// run the operator on both the min and the max, this gives us the [min, max] bound
	auto min = NumericStats::GetMin<TA>(nstats);
	auto max = NumericStats::GetMax<TA>(nstats);
	if (min > max) {
		return nullptr;
	}

	// Infinite values are unmodified
	auto min_part = TruncateFinite<TA, TR, OP>(min);
	auto max_part = TruncateFinite<TA, TR, OP>(max);

	auto min_value = Value::CreateValue(min_part);
	auto max_value = Value::CreateValue(max_part);
	auto result = NumericStats::CreateEmpty(min_value.type());
	NumericStats::SetMin(result, min_value);
	NumericStats::SetMax(result, max_value);

	result.CombineValidity(child_stats[0], child_stats[1]);
	return result.ToUnique();
}

template <class TA, class TR, class OP>
unique_ptr<BaseStatistics> PropagateDateTruncStatistics(ClientContext &context, FunctionStatisticsInput &input) {
	return DateTruncStatistics<TA, TR, OP>(input.child_stats);
}

template <typename TA, typename TR>
function_statistics_t DateTruncStats(DatePartSpecifier type) {
	return DateTrunc::Dispatch(
	    type, [](auto op) -> function_statistics_t { return PropagateDateTruncStatistics<TA, TR, decltype(op)>; });
}

unique_ptr<FunctionData> DateTruncBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	if (!arguments[0]->IsFoldable()) {
		return nullptr;
	}

	Value part_value = ExpressionExecutor::EvaluateScalar(context, *arguments[0]);
	if (part_value.IsNull()) {
		return nullptr;
	}
	const auto part_name = part_value.ToString();
	const auto part_code = GetDatePartSpecifier(part_name);

	switch (bound_function.GetArguments()[1].id()) {
	case LogicalType::TIMESTAMP:
		bound_function.SetFunctionCallback(DateTruncCallback<timestamp_t, timestamp_t>(part_code));
		bound_function.SetStatisticsCallback(DateTruncStats<timestamp_t, timestamp_t>(part_code));
		break;
	case LogicalType::DATE:
		bound_function.SetFunctionCallback(DateTruncCallback<date_t, timestamp_t>(part_code));
		bound_function.SetStatisticsCallback(DateTruncStats<date_t, timestamp_t>(part_code));
		break;
	default:
		throw NotImplementedException("Temporal argument type for DATETRUNC");
	}

	return nullptr;
}

} // namespace

ScalarFunctionSet DateTruncFun::GetFunctions() {
	ScalarFunctionSet date_trunc("date_trunc");
	date_trunc.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::TIMESTAMP}, LogicalType::TIMESTAMP,
	                                      DateTruncFunction<timestamp_t, timestamp_t>, DateTruncBind));
	date_trunc.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::DATE}, LogicalType::TIMESTAMP,
	                                      DateTruncFunction<date_t, timestamp_t>, DateTruncBind));
	date_trunc.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::INTERVAL}, LogicalType::INTERVAL,
	                                      DateTruncFunction<interval_t, interval_t>));
	for (auto &func : date_trunc.functions) {
		func.SetFallible();
		func.SetArgProperties(1, ArgProperties().NonDecreasing());
	}
	return date_trunc;
}

} // namespace duckdb
