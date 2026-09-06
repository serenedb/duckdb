#include "core_functions/scalar/date_functions.hpp"
#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/function/scalar/date_bucket_rewrite.hpp"
#include "duckdb/function/scalar/date_trunc_fast.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

namespace {

struct DateTruncBinaryOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA specifier, TB date) {
		return DateTrunc::Element<TB, TR>(GetDatePartSpecifier(specifier.GetString()), date);
	}
};

template <typename TA, typename TR>
void DateTruncUnaryExecutor(DatePartSpecifier type, const Vector &left, Vector &result) {
	DateTrunc::Dispatch(type, [&](auto op) {
		UnaryExecutor::Execute<TA, TR>(left, result, [](TA input) {
			return Value::IsFinite(input) ? decltype(op)::template Operation<TA, TR>(input)
			                              : Cast::template Operation<TA, TR>(input);
		});
	});
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
		DateTruncUnaryExecutor<TA, TR>(type, date_arg, result);
	} else {
		BinaryExecutor::ExecuteStandard<string_t, TA, TR, DateTruncBinaryOperator>(part_arg, date_arg, result);
	}
}

template <class TA, class TR, class OP>
TR TruncateBound(TA input) {
	return Value::IsFinite(input) ? OP::template Operation<TA, TR>(input) : Cast::template Operation<TA, TR>(input);
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
	auto min_part = TruncateBound<TA, TR, OP>(min);
	auto max_part = TruncateBound<TA, TR, OP>(max);

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
	auto &bound_function = input.GetBoundFunction();
	// Rebind to return a date if we are truncating that far - only possible if the part is a constant
	auto part_constant = input.TryGetConstant(0);
	if (!part_constant || part_constant->IsNull()) {
		return nullptr;
	}
	const auto part_name = part_constant->ToString();
	const auto part_code = GetDatePartSpecifier(part_name);

	switch (bound_function.GetArguments()[1].id()) {
	case LogicalType::TIMESTAMP:
		bound_function.SetStatisticsCallback(DateTruncStats<timestamp_t, timestamp_t>(part_code));
		bound_function.SetFunctionCallback(DateTruncFast::Callback<timestamp_t, timestamp_t>(part_code));
		break;
	case LogicalType::DATE:
		bound_function.SetStatisticsCallback(DateTruncStats<date_t, timestamp_t>(part_code));
		bound_function.SetFunctionCallback(DateTruncFast::Callback<date_t, timestamp_t>(part_code));
		break;
	default:
		throw NotImplementedException("Temporal argument type for DATETRUNC");
	}

	return nullptr;
}

} // namespace

// Names the "part,timestamp" pair shared by date_trunc's per-type overloads.
static ScalarFunction NamePartTimestampArguments(ScalarFunction fun, const LogicalType &type) {
	fun.GetSignature().AddParameter("part", LogicalType::VARCHAR).AddParameter("timestamp", type);
	return fun;
}

ScalarFunctionSet DateTruncFun::GetFunctions() {
	ScalarFunctionSet date_trunc("date_trunc");
	date_trunc.AddFunction(NamePartTimestampArguments(
	    ScalarFunction({}, LogicalType::TIMESTAMP, DateTruncFunction<timestamp_t, timestamp_t>, DateTruncBind),
	    LogicalType::TIMESTAMP));
	date_trunc.AddFunction(NamePartTimestampArguments(
	    ScalarFunction({}, LogicalType::TIMESTAMP, DateTruncFunction<date_t, timestamp_t>, DateTruncBind),
	    LogicalType::DATE));
	date_trunc.AddFunction(NamePartTimestampArguments(
	    ScalarFunction({}, LogicalType::INTERVAL, DateTruncFunction<interval_t, interval_t>), LogicalType::INTERVAL));
	date_trunc.ApplyToFunctions([](ScalarFunction &func) {
		func.SetFallible();
		func.SetArgProperties(1, ArgProperties().NonDecreasing());
		func.SetBucketRewriteCallback(DateTruncBucketRewrite);
	});
	return date_trunc;
}

} // namespace duckdb
