#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/operator/date_trunc_operators.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar/date_bucket_rewrite.hpp"
#include "duckdb/function/scalar/date_functions.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

namespace duckdb {

namespace {

bool TryGetConstant(const Expression &expr, int64_t &value) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	const auto &constant = expr.Cast<BoundConstantExpression>().GetValue();
	if (constant.IsNull()) {
		return false;
	}
	value = constant.GetValue<int64_t>();
	return true;
}

template <class INPUT>
int64_t Bucket(INPUT ts, int64_t width, int64_t anchor) {
	return DateTrunc::FloorDiv(ts.value - anchor, width);
}

template <class RESULT>
RESULT Unbucket(int64_t bucket, int64_t width, int64_t anchor) {
	return RESULT(bucket * width + anchor);
}

template <class INPUT>
int64_t MonthBucket(INPUT ts, int64_t width, int64_t anchor) {
	return DateTrunc::FloorDiv(DateTrunc::MonthIndex(timestamp_t(ts.value)) - anchor, width);
}

template <class RESULT>
RESULT MonthUnbucket(int64_t bucket, int64_t width, int64_t anchor) {
	return RESULT(DateTrunc::MonthIndexStart(bucket * width + anchor).value);
}

bool TryGetConstants(DataChunk &args, int64_t &width, int64_t &anchor) {
	auto &width_vector = args.data[1];
	auto &anchor_vector = args.data[2];
	if (width_vector.GetVectorType() != VectorType::CONSTANT_VECTOR ||
	    anchor_vector.GetVectorType() != VectorType::CONSTANT_VECTOR || ConstantVector::IsNull(width_vector) ||
	    ConstantVector::IsNull(anchor_vector)) {
		return false;
	}
	width = *ConstantVector::GetData<int64_t>(width_vector);
	anchor = *ConstantVector::GetData<int64_t>(anchor_vector);
	return true;
}

template <class INPUT, class RESULT, RESULT (*FUN)(INPUT, int64_t, int64_t)>
void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
	int64_t width = 0;
	int64_t anchor = 0;
	if (TryGetConstants(args, width, anchor)) {
		UnaryExecutor::Execute<INPUT, RESULT>(args.data[0], result, args.size(),
		                                      [&](INPUT input) { return FUN(input, width, anchor); });
		return;
	}
	TernaryExecutor::Execute<INPUT, int64_t, int64_t, RESULT>(args.data[0], args.data[1], args.data[2], result,
	                                                          args.size(), FUN);
}

template <class T>
bool Finite(T value) {
	return Value::IsFinite(value);
}

template <>
bool Finite(timestamp_tz_t value) {
	return value.IsFinite();
}

template <class INPUT, class RESULT, RESULT (*FUN)(INPUT, int64_t, int64_t)>
unique_ptr<BaseStatistics> RangeStatistics(ClientContext &context, FunctionStatisticsInput &input) {
	auto &children = input.expr.GetChildren();
	int64_t width = 0;
	int64_t anchor = 0;
	if (!TryGetConstant(*children[1], width) || !TryGetConstant(*children[2], anchor) || width <= 0) {
		return nullptr;
	}
	auto &child = input.child_stats[0];
	if (!NumericStats::HasMinMax(child)) {
		return nullptr;
	}
	const auto min = NumericStats::GetMin<INPUT>(child);
	const auto max = NumericStats::GetMax<INPUT>(child);
	if (min > max || !Finite(min) || !Finite(max)) {
		return nullptr;
	}
	auto result = NumericStats::CreateEmpty(input.expr.GetReturnType());
	result.CopyBase(child);
	NumericStats::SetMin(result, Value::CreateValue(FUN(min, width, anchor)));
	NumericStats::SetMax(result, Value::CreateValue(FUN(max, width, anchor)));
	return result.ToUnique();
}

template <class INPUT, class RESULT, RESULT (*FUN)(INPUT, int64_t, int64_t)>
ScalarFunction BucketFunction(const char *name, const LogicalType &input_type, const LogicalType &result_type) {
	return ScalarFunction(Identifier(name), {input_type, LogicalType::BIGINT, LogicalType::BIGINT}, result_type,
	                      Execute<INPUT, RESULT, FUN>, nullptr, RangeStatistics<INPUT, RESULT, FUN>);
}

ScalarFunction FunctionFor(const ScalarFunctionSet &set, LogicalTypeId input) {
	switch (input) {
	case LogicalTypeId::TIMESTAMP:
		return set.functions[0];
	case LogicalTypeId::TIMESTAMP_TZ:
		return set.functions[1];
	default:
		throw InternalException("No bucket function for input type %s", LogicalTypeIdToString(input));
	}
}

unique_ptr<Expression> MakeCall(ScalarFunction function, unique_ptr<Expression> input, const DateBucketSpec &spec) {
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(std::move(input));
	arguments.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(spec.width)));
	arguments.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(spec.anchor)));
	BoundScalarFunction bound_function(std::move(function));
	return make_uniq<BoundFunctionExpression>(std::move(bound_function), std::move(arguments), nullptr);
}

bool TryGetDateTruncSpec(DatePartSpecifier part, DateBucketSpec &spec) {
	spec = DateBucketSpec();
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

constexpr int64_t TIME_BUCKET_ORIGIN_MICROS = 10959 * Interval::MICROS_PER_DAY;
constexpr int64_t TIME_BUCKET_ORIGIN_MONTHS = 360;
constexpr int64_t EPOCH_MONTH_INDEX = int64_t(Date::EPOCH_YEAR) * Interval::MONTHS_PER_YEAR;

bool TryGetTimeBucketSpec(const vector<unique_ptr<Expression>> &children, DateBucketSpec &spec) {
	const auto &width_value = children[0]->Cast<BoundConstantExpression>().GetValue();
	if (width_value.IsNull() || width_value.type().id() != LogicalTypeId::INTERVAL) {
		return false;
	}
	const auto width = width_value.GetValue<interval_t>();
	spec = DateBucketSpec();
	if (width.months == 0 && Interval::GetMicro(width) > 0) {
		spec.width = Interval::GetMicro(width);
		spec.anchor = TIME_BUCKET_ORIGIN_MICROS % spec.width;
	} else if (width.months > 0 && width.days == 0 && width.micros == 0) {
		spec.calendar = true;
		spec.width = width.months;
		spec.anchor = EPOCH_MONTH_INDEX + TIME_BUCKET_ORIGIN_MONTHS % spec.width;
	} else {
		return false;
	}
	if (children.size() < 3) {
		return true;
	}
	if (children[2]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	const auto &third = children[2]->Cast<BoundConstantExpression>().GetValue();
	if (third.IsNull()) {
		return false;
	}
	switch (third.type().id()) {
	case LogicalTypeId::INTERVAL: {
		const auto offset = third.GetValue<interval_t>();
		if (spec.calendar || offset.months != 0) {
			return false;
		}
		spec.anchor += Interval::GetMicro(offset);
		return true;
	}
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::DATE: {
		int64_t origin = 0;
		if (third.type().id() == LogicalTypeId::TIMESTAMP) {
			const auto value = third.GetValue<timestamp_t>();
			if (!Value::IsFinite(value)) {
				return false;
			}
			origin = value.value;
		} else {
			const auto value = third.GetValue<date_t>();
			if (!Value::IsFinite(value)) {
				return false;
			}
			origin = int64_t(value.days) * Interval::MICROS_PER_DAY;
		}
		if (spec.calendar) {
			spec.anchor = EPOCH_MONTH_INDEX + (DateTrunc::MonthIndex(timestamp_t(origin)) - EPOCH_MONTH_INDEX) % spec.width;
		} else {
			spec.anchor = origin % spec.width;
		}
		return true;
	}
	default:
		return false;
	}
}

} // namespace

unique_ptr<BucketRewrite> DateTruncBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr) {
	auto &children = expr.GetChildren();
	if (children.size() != 2 || children[0]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return nullptr;
	}
	const auto &input_type = children[1]->GetReturnType();
	if (input_type.id() != LogicalTypeId::TIMESTAMP && input_type.id() != LogicalTypeId::DATE) {
		return nullptr;
	}
	const auto &unit = children[0]->Cast<BoundConstantExpression>().GetValue();
	DatePartSpecifier part;
	DateBucketSpec spec;
	if (unit.IsNull() || !TryGetDatePartSpecifier(StringValue::Get(unit), part) || !TryGetDateTruncSpec(part, spec)) {
		return nullptr;
	}
	return make_uniq<DateBucketRewrite>(context, spec, 1, input_type, expr.GetReturnType(), true);
}

unique_ptr<BucketRewrite> TimeBucketBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr) {
	auto &children = expr.GetChildren();
	if (children.size() < 2 || children.size() > 3 ||
	    children[0]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return nullptr;
	}
	const auto &input_type = children[1]->GetReturnType();
	if (input_type.id() != LogicalTypeId::TIMESTAMP && input_type.id() != LogicalTypeId::DATE) {
		return nullptr;
	}
	DateBucketSpec spec;
	if (!TryGetTimeBucketSpec(children, spec)) {
		return nullptr;
	}
	return make_uniq<DateBucketRewrite>(context, spec, 1, input_type, expr.GetReturnType(), false);
}

FunctionBucketRewrite::FunctionBucketRewrite(unique_ptr<BucketRewrite> inner_p, const BoundFunctionExpression &expr,
                                             idx_t input_index_p)
    : inner(std::move(inner_p)), function(expr.Function()),
      bind_info(expr.BindInfo() ? expr.BindInfo()->Copy() : nullptr), input_index(input_index_p) {
	for (auto &child : expr.GetChildren()) {
		arguments.push_back(child->Copy());
	}
}

idx_t FunctionBucketRewrite::InputIndex() const {
	return input_index;
}

bool FunctionBucketRewrite::TryBucketRange(const BaseStatistics &input_stats, int64_t &min_bucket,
                                           int64_t &max_bucket) const {
	return inner->TryBucketRange(input_stats, min_bucket, max_bucket);
}

unique_ptr<Expression> FunctionBucketRewrite::Bucket(unique_ptr<Expression> input) const {
	return inner->Bucket(std::move(input));
}

unique_ptr<Expression> FunctionBucketRewrite::Unbucket(unique_ptr<Expression> bucket) const {
	vector<unique_ptr<Expression>> children;
	for (idx_t i = 0; i < arguments.size(); i++) {
		children.push_back(i == input_index ? inner->Unbucket(std::move(bucket)) : arguments[i]->Copy());
	}
	return make_uniq<BoundFunctionExpression>(function, std::move(children), bind_info ? bind_info->Copy() : nullptr);
}

CyclicBucketRewrite::CyclicBucketRewrite(ScalarFunction unbucket_function_p, int64_t min_bucket_p,
                                         int64_t max_bucket_p)
    : unbucket_function(std::move(unbucket_function_p)), min_bucket(min_bucket_p), max_bucket(max_bucket_p) {
}

idx_t CyclicBucketRewrite::InputIndex() const {
	return 0;
}

bool CyclicBucketRewrite::TryBucketRange(const BaseStatistics &input_stats, int64_t &min_bucket_p,
                                         int64_t &max_bucket_p) const {
	min_bucket_p = min_bucket;
	max_bucket_p = max_bucket;
	return true;
}

unique_ptr<Expression> CyclicBucketRewrite::Unbucket(unique_ptr<Expression> bucket) const {
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(std::move(bucket));
	BoundScalarFunction bound_function(unbucket_function);
	return make_uniq<BoundFunctionExpression>(std::move(bound_function), std::move(arguments), nullptr);
}

namespace {

struct StrfTimeInspector : public StrfTimeFormat {
	using StrfTimeFormat::specifiers;
};

} // namespace

bool TryGetStrfTimeGranularity(const string &format_string, bool sub_day_constant, DatePartSpecifier &part) {
	StrfTimeInspector format;
	if (!StrTimeFormat::ParseFormatSpecifier(format_string, format).empty()) {
		return false;
	}
	enum class Level : uint8_t { NONE, YEAR, MONTH, DAY, HOUR, MINUTE, SECOND };
	auto finest = Level::NONE;
	auto raise = [&](Level level) {
		if (level > finest) {
			finest = level;
		}
	};
	bool year = false;
	bool month = false;
	bool day = false;
	bool day_of_year = false;
	bool hour24 = false;
	bool hour12 = false;
	bool am_pm = false;
	bool minute = false;
	bool second = false;
	for (const auto specifier : format.specifiers) {
		switch (specifier) {
		case StrTimeSpecifier::YEAR_DECIMAL:
			year = true;
			raise(Level::YEAR);
			break;
		case StrTimeSpecifier::ABBREVIATED_MONTH_NAME:
		case StrTimeSpecifier::FULL_MONTH_NAME:
		case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
		case StrTimeSpecifier::MONTH_DECIMAL:
			month = true;
			raise(Level::MONTH);
			break;
		case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
		case StrTimeSpecifier::DAY_OF_MONTH:
			day = true;
			raise(Level::DAY);
			break;
		case StrTimeSpecifier::DAY_OF_YEAR_PADDED:
		case StrTimeSpecifier::DAY_OF_YEAR_DECIMAL:
			day_of_year = true;
			raise(Level::DAY);
			break;
		case StrTimeSpecifier::ABBREVIATED_WEEKDAY_NAME:
		case StrTimeSpecifier::FULL_WEEKDAY_NAME:
		case StrTimeSpecifier::WEEKDAY_DECIMAL:
		case StrTimeSpecifier::WEEKDAY_ISO:
			raise(Level::DAY);
			break;
		case StrTimeSpecifier::HOUR_24_PADDED:
		case StrTimeSpecifier::HOUR_24_DECIMAL:
			hour24 = true;
			raise(Level::HOUR);
			break;
		case StrTimeSpecifier::HOUR_12_PADDED:
		case StrTimeSpecifier::HOUR_12_DECIMAL:
			hour12 = true;
			raise(Level::HOUR);
			break;
		case StrTimeSpecifier::AM_PM:
			am_pm = true;
			raise(Level::HOUR);
			break;
		case StrTimeSpecifier::MINUTE_PADDED:
		case StrTimeSpecifier::MINUTE_DECIMAL:
			minute = true;
			raise(Level::MINUTE);
			break;
		case StrTimeSpecifier::SECOND_PADDED:
		case StrTimeSpecifier::SECOND_DECIMAL:
			second = true;
			raise(Level::SECOND);
			break;
		default:
			return false;
		}
	}
	if (sub_day_constant && finest > Level::DAY) {
		finest = Level::DAY;
	}
	if (finest == Level::NONE || !year) {
		return false;
	}
	if (finest >= Level::MONTH && !month && !day_of_year) {
		return false;
	}
	if (finest >= Level::DAY && !(month && day) && !day_of_year) {
		return false;
	}
	if (finest >= Level::HOUR && !hour24 && !(hour12 && am_pm)) {
		return false;
	}
	if (finest >= Level::MINUTE && !minute) {
		return false;
	}
	if (finest >= Level::SECOND && !second) {
		return false;
	}
	switch (finest) {
	case Level::YEAR:
		part = DatePartSpecifier::YEAR;
		return true;
	case Level::MONTH:
		part = DatePartSpecifier::MONTH;
		return true;
	case Level::DAY:
		part = DatePartSpecifier::DAY;
		return true;
	case Level::HOUR:
		part = DatePartSpecifier::HOUR;
		return true;
	case Level::MINUTE:
		part = DatePartSpecifier::MINUTE;
		return true;
	default:
		part = DatePartSpecifier::SECOND;
		return true;
	}
}

unique_ptr<BucketRewrite> StrfTimeBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr) {
	auto &children = expr.GetChildren();
	if (children.size() != 2) {
		return nullptr;
	}
	idx_t input_index = 0;
	for (; input_index < 2; input_index++) {
		const auto id = children[input_index]->GetReturnType().id();
		if (id == LogicalTypeId::TIMESTAMP || id == LogicalTypeId::DATE) {
			break;
		}
	}
	if (input_index == 2) {
		return nullptr;
	}
	auto &format_expr = *children[1 - input_index];
	if (format_expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
	    format_expr.GetReturnType().id() != LogicalTypeId::VARCHAR) {
		return nullptr;
	}
	const auto &format_value = format_expr.Cast<BoundConstantExpression>().GetValue();
	if (format_value.IsNull()) {
		return nullptr;
	}
	const auto &input_type = children[input_index]->GetReturnType();
	DatePartSpecifier part;
	DateBucketSpec spec;
	if (!TryGetStrfTimeGranularity(StringValue::Get(format_value), input_type.id() == LogicalTypeId::DATE, part) ||
	    !TryGetDateTruncSpec(part, spec)) {
		return nullptr;
	}
	auto inner = make_uniq<DateBucketRewrite>(context, spec, input_index, input_type, input_type, false);
	return make_uniq<FunctionBucketRewrite>(std::move(inner), expr, input_index);
}

namespace {

template <class INPUT>
int64_t DaysOf(INPUT input);

template <>
int64_t DaysOf(date_t input) {
	return input.days;
}

template <>
int64_t DaysOf(timestamp_t input) {
	return DateTrunc::ToDays(input);
}

template <class INPUT>
void MonthOfYearFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<INPUT, int64_t>(args.data[0], result, args.size(), [&](INPUT input) -> optional<int64_t> {
		if (!Value::IsFinite(input)) {
			return nullopt;
		}
		const auto month = DateTrunc::MonthIndex(DaysOf(input));
		return month - DateTrunc::FloorDiv(month, int64_t(12)) * 12 + 1;
	});
}

template <class INPUT>
void DayOfWeekFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<INPUT, int64_t>(args.data[0], result, args.size(), [&](INPUT input) -> optional<int64_t> {
		if (!Value::IsFinite(input)) {
			return nullopt;
		}
		return int64_t(Date::ExtractISODayOfTheWeek(date_t(UnsafeNumericCast<int32_t>(DaysOf(input)))) % 7);
	});
}

void MonthNameFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<int64_t, string_t>(args.data[0], result, args.size(), [&](int64_t month) {
		if (month < 1 || month > 12) {
			throw InvalidInputException("Month bucket %d outside 1..12", month);
		}
		return Date::MONTH_NAMES[month - 1];
	});
}

void DayNameFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<int64_t, string_t>(args.data[0], result, args.size(), [&](int64_t day) {
		if (day < 0 || day > 6) {
			throw InvalidInputException("Day bucket %d outside 0..6", day);
		}
		return Date::DAY_NAMES[day];
	});
}

class DateCyclicBucketRewrite : public CyclicBucketRewrite {
public:
	DateCyclicBucketRewrite(ScalarFunction bucket_function_p, ScalarFunction unbucket_function_p, int64_t min_bucket_p,
	                        int64_t max_bucket_p)
	    : CyclicBucketRewrite(std::move(unbucket_function_p), min_bucket_p, max_bucket_p),
	      bucket_function(std::move(bucket_function_p)) {
	}

	unique_ptr<Expression> Bucket(unique_ptr<Expression> input) const override {
		vector<unique_ptr<Expression>> arguments;
		arguments.push_back(std::move(input));
		BoundScalarFunction bound_function(bucket_function);
		return make_uniq<BoundFunctionExpression>(std::move(bound_function), std::move(arguments), nullptr);
	}

private:
	ScalarFunction bucket_function;
};

unique_ptr<BucketRewrite> CyclicRewrite(const BoundFunctionExpression &expr, const ScalarFunctionSet &buckets,
                                        ScalarFunction unbucket, int64_t min_bucket, int64_t max_bucket) {
	auto &children = expr.GetChildren();
	if (children.size() != 1) {
		return nullptr;
	}
	switch (children[0]->GetReturnType().id()) {
	case LogicalTypeId::DATE:
		return make_uniq<DateCyclicBucketRewrite>(buckets.functions[0], std::move(unbucket), min_bucket, max_bucket);
	case LogicalTypeId::TIMESTAMP:
		return make_uniq<DateCyclicBucketRewrite>(buckets.functions[1], std::move(unbucket), min_bucket, max_bucket);
	default:
		return nullptr;
	}
}

} // namespace

unique_ptr<BucketRewrite> MonthNameBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr) {
	return CyclicRewrite(expr, InternalMonthOfYearFun::GetFunctions(), InternalMonthNameFun::GetFunction(), 1, 12);
}

unique_ptr<BucketRewrite> DayNameBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr) {
	return CyclicRewrite(expr, InternalDayOfWeekFun::GetFunctions(), InternalDayNameFun::GetFunction(), 0, 6);
}

ScalarFunctionSet InternalMonthOfYearFun::GetFunctions() {
	ScalarFunctionSet set(Name);
	set.AddFunction(ScalarFunction(Identifier(Name), {LogicalType::DATE}, LogicalType::BIGINT, MonthOfYearFunction<date_t>));
	set.AddFunction(
	    ScalarFunction(Identifier(Name), {LogicalType::TIMESTAMP}, LogicalType::BIGINT, MonthOfYearFunction<timestamp_t>));
	return set;
}

ScalarFunctionSet InternalDayOfWeekFun::GetFunctions() {
	ScalarFunctionSet set(Name);
	set.AddFunction(ScalarFunction(Identifier(Name), {LogicalType::DATE}, LogicalType::BIGINT, DayOfWeekFunction<date_t>));
	set.AddFunction(
	    ScalarFunction(Identifier(Name), {LogicalType::TIMESTAMP}, LogicalType::BIGINT, DayOfWeekFunction<timestamp_t>));
	return set;
}

ScalarFunction InternalMonthNameFun::GetFunction() {
	return ScalarFunction(Identifier(Name), {LogicalType::BIGINT}, LogicalType::VARCHAR, MonthNameFunction);
}

ScalarFunction InternalDayNameFun::GetFunction() {
	return ScalarFunction(Identifier(Name), {LogicalType::BIGINT}, LogicalType::VARCHAR, DayNameFunction);
}

int64_t DateBucketSpec::Bucket(int64_t micros) const {
	return DateTrunc::FloorDiv((calendar ? DateTrunc::MonthIndex(timestamp_t(micros)) : micros) - anchor, width);
}

DateBucketRewrite::DateBucketRewrite(ClientContext &context_p, DateBucketSpec spec_p, idx_t input_index_p,
                                     LogicalType input_type_p, LogicalType result_type_p, bool anno_domini_only_p)
    : context(context_p), spec(spec_p), input_index(input_index_p), input_type(std::move(input_type_p)),
      result_type(std::move(result_type_p)), anno_domini_only(anno_domini_only_p) {
}

idx_t DateBucketRewrite::InputIndex() const {
	return input_index;
}

bool DateBucketRewrite::TryBucketRange(const BaseStatistics &stats, int64_t &min_bucket, int64_t &max_bucket) const {
	if (!NumericStats::HasMinMax(stats)) {
		return false;
	}
	int64_t min = 0;
	int64_t max = 0;
	switch (input_type.id()) {
	case LogicalTypeId::TIMESTAMP: {
		const auto lo = NumericStats::GetMin<timestamp_t>(stats);
		const auto hi = NumericStats::GetMax<timestamp_t>(stats);
		if (!Value::IsFinite(lo) || !Value::IsFinite(hi)) {
			return false;
		}
		min = lo.value;
		max = hi.value;
		break;
	}
	case LogicalTypeId::TIMESTAMP_TZ: {
		const auto lo = NumericStats::GetMin<timestamp_tz_t>(stats);
		const auto hi = NumericStats::GetMax<timestamp_tz_t>(stats);
		if (!lo.IsFinite() || !hi.IsFinite()) {
			return false;
		}
		min = lo.value;
		max = hi.value;
		break;
	}
	case LogicalTypeId::DATE: {
		const auto lo = NumericStats::GetMin<date_t>(stats);
		const auto hi = NumericStats::GetMax<date_t>(stats);
		if (!Value::IsFinite(lo) || !Value::IsFinite(hi)) {
			return false;
		}
		min = int64_t(lo.days) * Interval::MICROS_PER_DAY;
		max = int64_t(hi.days) * Interval::MICROS_PER_DAY;
		break;
	}
	default:
		return false;
	}
	const auto limit = NumericLimits<int64_t>::Maximum() - 2 * Interval::MICROS_PER_WEEK;
	const auto lower =
	    anno_domini_only && spec.calendar ? DateTrunc::YearStart(1) * Interval::MICROS_PER_DAY : -limit;
	if (min > max || min < lower || max > limit) {
		return false;
	}
	min_bucket = spec.Bucket(min);
	max_bucket = spec.Bucket(max);
	return true;
}

unique_ptr<Expression> DateBucketRewrite::Bucket(unique_ptr<Expression> input) const {
	auto type = input_type.id();
	if (type == LogicalTypeId::DATE) {
		input = BoundCastExpression::AddCastToType(context, std::move(input), LogicalType::TIMESTAMP);
		type = LogicalTypeId::TIMESTAMP;
	}
	const auto set =
	    spec.calendar ? InternalDateTruncMonthBucketFun::GetFunctions() : InternalDateTruncBucketFun::GetFunctions();
	return MakeCall(FunctionFor(set, type), std::move(input), spec);
}

unique_ptr<Expression> DateBucketRewrite::Unbucket(unique_ptr<Expression> bucket) const {
	const bool zoned = result_type.id() == LogicalTypeId::TIMESTAMP_TZ;
	ScalarFunction function = spec.calendar ? (zoned ? InternalDateTruncMonthUnbucketTzFun::GetFunction()
	                                                 : InternalDateTruncMonthUnbucketFun::GetFunction())
	                                        : (zoned ? InternalDateTruncUnbucketTzFun::GetFunction()
	                                                 : InternalDateTruncUnbucketFun::GetFunction());
	auto expr = MakeCall(std::move(function), std::move(bucket), spec);
	if (result_type.id() == LogicalTypeId::DATE) {
		expr = BoundCastExpression::AddCastToType(context, std::move(expr), LogicalType::DATE);
	}
	return expr;
}

ScalarFunctionSet InternalDateTruncBucketFun::GetFunctions() {
	ScalarFunctionSet set(Name);
	set.AddFunction(BucketFunction<timestamp_t, int64_t, Bucket<timestamp_t>>(Name, LogicalType::TIMESTAMP,
	                                                                          LogicalType::BIGINT));
	set.AddFunction(BucketFunction<timestamp_tz_t, int64_t, Bucket<timestamp_tz_t>>(Name, LogicalType::TIMESTAMP_TZ,
	                                                                                LogicalType::BIGINT));
	return set;
}

ScalarFunctionSet InternalDateTruncMonthBucketFun::GetFunctions() {
	ScalarFunctionSet set(Name);
	set.AddFunction(BucketFunction<timestamp_t, int64_t, MonthBucket<timestamp_t>>(Name, LogicalType::TIMESTAMP,
	                                                                               LogicalType::BIGINT));
	set.AddFunction(BucketFunction<timestamp_tz_t, int64_t, MonthBucket<timestamp_tz_t>>(
	    Name, LogicalType::TIMESTAMP_TZ, LogicalType::BIGINT));
	return set;
}

ScalarFunction InternalDateTruncUnbucketFun::GetFunction() {
	return BucketFunction<int64_t, timestamp_t, Unbucket<timestamp_t>>(Name, LogicalType::BIGINT,
	                                                                   LogicalType::TIMESTAMP);
}

ScalarFunction InternalDateTruncUnbucketTzFun::GetFunction() {
	return BucketFunction<int64_t, timestamp_tz_t, Unbucket<timestamp_tz_t>>(Name, LogicalType::BIGINT,
	                                                                         LogicalType::TIMESTAMP_TZ);
}

ScalarFunction InternalDateTruncMonthUnbucketFun::GetFunction() {
	return BucketFunction<int64_t, timestamp_t, MonthUnbucket<timestamp_t>>(Name, LogicalType::BIGINT,
	                                                                        LogicalType::TIMESTAMP);
}

ScalarFunction InternalDateTruncMonthUnbucketTzFun::GetFunction() {
	return BucketFunction<int64_t, timestamp_tz_t, MonthUnbucket<timestamp_tz_t>>(Name, LogicalType::BIGINT,
	                                                                              LogicalType::TIMESTAMP_TZ);
}

} // namespace duckdb
