#include "core_functions/scalar/date_functions.hpp"
#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/operator/date_trunc_operators.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar/date_functions.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

namespace duckdb {

namespace {

struct BucketSpec {
	bool calendar = false;
	int64_t width = 0;
	int64_t anchor = 0;

	int64_t Bucket(timestamp_t ts) const {
		return DateTrunc::FloorDiv((calendar ? DateTrunc::MonthIndex(ts) : ts.value) - anchor, width);
	}
	ScalarFunction BucketFunction() const {
		return calendar ? InternalDateTruncMonthBucketFun::GetFunction() : InternalDateTruncBucketFun::GetFunction();
	}
	ScalarFunction UnbucketFunction() const {
		return calendar ? InternalDateTruncMonthUnbucketFun::GetFunction()
		                : InternalDateTruncUnbucketFun::GetFunction();
	}
};

bool TryGetBucketSpec(DatePartSpecifier part, BucketSpec &spec) {
	spec = BucketSpec();
	switch (part) {
	case DatePartSpecifier::SECOND:
	case DatePartSpecifier::EPOCH:
		spec.width = Interval::MICROS_PER_SEC;
		return true;
	case DatePartSpecifier::MINUTE:
		spec.width = Interval::MICROS_PER_MINUTE;
		return true;
	case DatePartSpecifier::HOUR:
		spec.width = Interval::MICROS_PER_HOUR;
		return true;
	case DatePartSpecifier::DAY:
	case DatePartSpecifier::DOW:
	case DatePartSpecifier::ISODOW:
	case DatePartSpecifier::DOY:
	case DatePartSpecifier::JULIAN_DAY:
		spec.width = Interval::MICROS_PER_DAY;
		return true;
	case DatePartSpecifier::WEEK:
	case DatePartSpecifier::YEARWEEK:
		spec.width = Interval::MICROS_PER_WEEK;
		spec.anchor = DateTrunc::EPOCH_MONDAY * Interval::MICROS_PER_DAY;
		return true;
	case DatePartSpecifier::MONTH:
		spec.calendar = true;
		spec.width = 1;
		return true;
	case DatePartSpecifier::QUARTER:
		spec.calendar = true;
		spec.width = Interval::MONTHS_PER_QUARTER;
		return true;
	case DatePartSpecifier::YEAR:
		spec.calendar = true;
		spec.width = Interval::MONTHS_PER_YEAR;
		return true;
	case DatePartSpecifier::DECADE:
		spec.calendar = true;
		spec.width = Interval::MONTHS_PER_DECADE;
		return true;
	case DatePartSpecifier::CENTURY:
		spec.calendar = true;
		spec.width = Interval::MONTHS_PER_CENTURY;
		return true;
	case DatePartSpecifier::MILLENNIUM:
		spec.calendar = true;
		spec.width = Interval::MONTHS_PER_MILLENIUM;
		return true;
	default:
		return false;
	}
}

unique_ptr<Expression> MakeBucketCall(ScalarFunction function, unique_ptr<Expression> input, const BucketSpec &spec) {
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(std::move(input));
	arguments.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(spec.width)));
	arguments.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(spec.anchor)));
	BoundScalarFunction bound_function(std::move(function));
	return make_uniq<BoundFunctionExpression>(std::move(bound_function), std::move(arguments), nullptr);
}

class DateTruncBucketRewrite : public BucketRewrite {
public:
	explicit DateTruncBucketRewrite(BucketSpec spec_p) : spec(spec_p) {
	}

	idx_t InputIndex() const override {
		return 1;
	}
	bool TryBucketRange(const BaseStatistics &input_stats, int64_t &min_bucket, int64_t &max_bucket) const override {
		if (!NumericStats::HasMinMax(input_stats)) {
			return false;
		}
		const auto limit = NumericLimits<int64_t>::Maximum() - 2 * Interval::MICROS_PER_WEEK;
		const auto lower = spec.calendar ? DateTrunc::YearStart(1) * Interval::MICROS_PER_DAY : -limit;
		const auto min = NumericStats::GetMin<timestamp_t>(input_stats);
		const auto max = NumericStats::GetMax<timestamp_t>(input_stats);
		if (min > max || min.value < lower || max.value > limit) {
			return false;
		}
		min_bucket = spec.Bucket(min);
		max_bucket = spec.Bucket(max);
		return true;
	}
	unique_ptr<Expression> Bucket(unique_ptr<Expression> input) const override {
		return MakeBucketCall(spec.BucketFunction(), std::move(input), spec);
	}
	unique_ptr<Expression> Unbucket(unique_ptr<Expression> bucket) const override {
		return MakeBucketCall(spec.UnbucketFunction(), std::move(bucket), spec);
	}

private:
	BucketSpec spec;
};

unique_ptr<BucketRewrite> DateTruncBucket(ClientContext &context, const BoundFunctionExpression &expr) {
	auto &children = expr.GetChildren();
	if (children.size() != 2 || children[0]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
	    children[1]->GetReturnType().id() != LogicalTypeId::TIMESTAMP) {
		return nullptr;
	}
	const auto &unit = children[0]->Cast<BoundConstantExpression>().GetValue();
	DatePartSpecifier part;
	BucketSpec spec;
	if (unit.IsNull() || !TryGetDatePartSpecifier(StringValue::Get(unit), part) || !TryGetBucketSpec(part, spec)) {
		return nullptr;
	}
	return make_uniq<DateTruncBucketRewrite>(spec);
}

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
	ScalarFunction timestamp_trunc({LogicalType::VARCHAR, LogicalType::TIMESTAMP}, LogicalType::TIMESTAMP,
	                               DateTruncFunction<timestamp_t, timestamp_t>, DateTruncBind);
	timestamp_trunc.SetBucketRewriteCallback(DateTruncBucket);
	date_trunc.AddFunction(std::move(timestamp_trunc));
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
