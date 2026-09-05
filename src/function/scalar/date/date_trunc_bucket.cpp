////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2026 SereneDB GmbH, Berlin, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is SereneDB GmbH, Berlin, Germany
////////////////////////////////////////////////////////////////////////////////

#include <absl/algorithm/container.h>

#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/date_trunc_operators.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/date_bucket_rewrite.hpp"
#include "duckdb/function/scalar/date_functions.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

#include <numeric>

namespace duckdb {

namespace {

bool TryGetConstant(const Expression &expr, int64_t &value) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	const auto &constant = expr.Cast<BoundConstantExpression>().GetValue();
	if (constant.IsNull() || constant.type().id() != LogicalTypeId::BIGINT) {
		return false;
	}
	value = BigIntValue::Get(constant);
	return true;
}

int64_t RawValue(timestamp_t value) {
	return value.value;
}

int64_t RawValue(timestamp_tz_t value) {
	return value.value;
}

int64_t RawValue(timestamp_ms_t value) {
	return value.value;
}

int64_t RawValue(timestamp_sec_t value) {
	return value.value;
}

int64_t RawValue(timestamp_ns_t value) {
	return value.value;
}

int64_t RawValue(date_t value) {
	return value.days;
}

int64_t RawValue(dtime_t value) {
	return value.micros;
}

template <class INPUT>
int64_t Micros(INPUT ts) {
	return RawValue(ts);
}

template <>
int64_t Micros(timestamp_ns_t ts) {
	return DateTrunc::FloorDiv(RawValue(ts), Interval::NANOS_PER_MICRO);
}

void ValidateWidth(int64_t width) {
	if (width <= 0) {
		throw InvalidInputException("Bucket width must be positive");
	}
}

int64_t Origin(int64_t bucket, int64_t width, int64_t anchor) {
	int64_t result = 0;
	if (!TryMultiplyOperator::Operation(bucket, width, result) || !TryAddOperator::Operation(result, anchor, result)) {
		throw OutOfRangeException("Bucket %lld of width %lld is out of range", bucket, width);
	}
	return result;
}

template <class INPUT>
int64_t Bucket(INPUT ts, int64_t width, int64_t anchor) {
	return DateTrunc::FloorDiv(Micros(ts) - anchor, width);
}

template <class RESULT>
RESULT Unbucket(int64_t bucket, int64_t width, int64_t anchor) {
	return RESULT(Origin(bucket, width, anchor));
}

template <>
dtime_t Unbucket<dtime_t>(int64_t bucket, int64_t width, int64_t anchor) {
	const auto micros = Origin(bucket, width, anchor);
	if (micros < 0 || micros >= Interval::MICROS_PER_DAY) {
		throw OutOfRangeException("Bucket %lld of width %lld is not a time of day", bucket, width);
	}
	return dtime_t(micros);
}

template <class INPUT>
int64_t MonthBucket(INPUT ts, int64_t width, int64_t anchor) {
	return DateTrunc::FloorDiv(DateTrunc::MonthIndex(timestamp_t(Micros(ts))) - anchor, width);
}

template <class RESULT>
RESULT MonthUnbucket(int64_t bucket, int64_t width, int64_t anchor) {
	return RESULT(DateTrunc::MonthIndexStart(Origin(bucket, width, anchor)).value);
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
		ValidateWidth(width);
		UnaryExecutor::Execute<INPUT, RESULT>(args.data[0], result, args.size(),
		                                      [&](INPUT input) { return FUN(input, width, anchor); });
		return;
	}
	TernaryExecutor::Execute<INPUT, int64_t, int64_t, RESULT>(args.data[0], args.data[1], args.data[2], result,
	                                                          args.size(),
	                                                          [](INPUT input, int64_t row_width, int64_t row_anchor) {
		                                                          ValidateWidth(row_width);
		                                                          return FUN(input, row_width, row_anchor);
	                                                          });
}

template <class T>
bool Finite(T value) {
	return Value::IsFinite(value);
}

template <>
bool Finite(timestamp_tz_t value) {
	return value.IsFinite();
}

template <>
bool Finite(dtime_t value) {
	return true;
}

template <class T>
bool TryScaledRange(const BaseStatistics &stats, int64_t scale, int64_t limit, int64_t &min, int64_t &max) {
	const auto lo = NumericStats::GetMin<T>(stats);
	const auto hi = NumericStats::GetMax<T>(stats);
	if (!Finite(lo) || !Finite(hi)) {
		return false;
	}
	const auto lo_raw = RawValue(lo);
	const auto hi_raw = RawValue(hi);
	if (lo_raw < -limit / scale || hi_raw > limit / scale) {
		return false;
	}
	min = lo_raw * scale;
	max = hi_raw * scale;
	return true;
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
	Value min_bucket;
	Value max_bucket;
	try {
		min_bucket = Value::CreateValue(FUN(min, width, anchor));
		max_bucket = Value::CreateValue(FUN(max, width, anchor));
	} catch (const OutOfRangeException &) {
		return nullptr;
	}
	auto result = NumericStats::CreateEmpty(input.expr.GetReturnType());
	result.CopyBase(child);
	NumericStats::SetMin(result, std::move(min_bucket));
	NumericStats::SetMax(result, std::move(max_bucket));
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
	case LogicalTypeId::TIMESTAMP_NS:
		return set.functions[2];
	case LogicalTypeId::TIME:
		return set.functions[3];
	default:
		throw InternalException("No bucket function for input type %s", LogicalTypeIdToString(input));
	}
}

unique_ptr<Expression> MakeCall(const ScalarFunction &function, unique_ptr<Expression> input,
                                const DateBucketSpec &spec) {
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(std::move(input));
	arguments.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(spec.width)));
	arguments.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(spec.anchor)));
	return MakeBucketCall(function, std::move(arguments));
}

unique_ptr<Expression> MakeUnaryCall(const ScalarFunction &function, unique_ptr<Expression> input) {
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(std::move(input));
	return MakeBucketCall(function, std::move(arguments));
}

bool TryGetDateTruncSpec(DatePartSpecifier part, DateBucketSpec &spec) {
	spec = DateBucketSpec();
	DateTruncUnit unit;
	if (!DateTrunc::TryGetUnit(part, unit)) {
		return false;
	}
	const auto scale = unit.unit == DateTruncUnit::Unit::DAYS ? Interval::MICROS_PER_DAY : 1;
	spec.calendar = unit.unit == DateTruncUnit::Unit::MONTHS;
	spec.width = unit.width * scale;
	spec.anchor = unit.anchor * scale;
	return true;
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
			spec.anchor =
			    EPOCH_MONTH_INDEX + (DateTrunc::MonthIndex(timestamp_t(origin)) - EPOCH_MONTH_INDEX) % spec.width;
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

unique_ptr<Expression> ReplaceEqualExpression(unique_ptr<Expression> expr, const Expression &target,
                                              const Expression &replacement) {
	if (expr->Equals(target)) {
		return replacement.Copy();
	}
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		child = ReplaceEqualExpression(std::move(child), target, replacement);
	});
	return expr;
}

unique_ptr<Expression> RebuildShell(ClientContext &context, const Expression &shell, const Expression &template_input,
                                    unique_ptr<Expression> value) {
	value = BoundCastExpression::AddCastToType(context, std::move(value), template_input.GetReturnType());
	return ReplaceEqualExpression(shell.Copy(), template_input, *value);
}

unique_ptr<Expression> MakeBucketCall(const ScalarFunction &function, vector<unique_ptr<Expression>> arguments,
                                      unique_ptr<FunctionData> bind_info) {
	BoundScalarFunction bound_function(function);
	return make_uniq<BoundFunctionExpression>(std::move(bound_function), std::move(arguments), std::move(bind_info));
}

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
	const bool timed = input_type.id() == LogicalTypeId::TIME;
	if (input_type.id() != LogicalTypeId::TIMESTAMP && input_type.id() != LogicalTypeId::DATE && !timed) {
		return nullptr;
	}
	if (timed && children.size() != 2) {
		return nullptr;
	}
	DateBucketSpec spec;
	if (!TryGetTimeBucketSpec(children, spec)) {
		return nullptr;
	}
	if (timed &&
	    (spec.calendar || spec.width > Interval::MICROS_PER_DAY || Interval::MICROS_PER_DAY % spec.width != 0)) {
		return nullptr;
	}
	return make_uniq<DateBucketRewrite>(context, spec, 1, input_type, expr.GetReturnType(), false);
}

FunctionBucketRewrite::FunctionBucketRewrite(unique_ptr<BucketRewrite> inner_p, const BoundFunctionExpression &expr,
                                             idx_t input_index_p)
    : DelegatingBucketRewrite(std::move(inner_p)), function(expr.Function()),
      bind_info(expr.BindInfo() ? expr.BindInfo()->Copy() : nullptr), input_index(input_index_p) {
	for (auto &child : expr.GetChildren()) {
		arguments.push_back(child->Copy());
	}
}

idx_t FunctionBucketRewrite::InputIndex() const {
	return input_index;
}

unique_ptr<Expression> FunctionBucketRewrite::Unbucket(unique_ptr<Expression> bucket) const {
	vector<unique_ptr<Expression>> children;
	for (idx_t i = 0; i < arguments.size(); i++) {
		children.push_back(i == input_index ? inner->Unbucket(std::move(bucket)) : arguments[i]->Copy());
	}
	return make_uniq<BoundFunctionExpression>(function, std::move(children), bind_info ? bind_info->Copy() : nullptr);
}

CyclicBucketRewrite::CyclicBucketRewrite(ScalarFunction unbucket_function_p, int64_t min_bucket_p, int64_t max_bucket_p)
    : unbucket_function(std::move(unbucket_function_p)), min_bucket(min_bucket_p), max_bucket(max_bucket_p) {
}

idx_t CyclicBucketRewrite::InputIndex() const {
	return 0;
}

bool CyclicBucketRewrite::TryConstantRange(int64_t &min_bucket_p, int64_t &max_bucket_p) const {
	min_bucket_p = min_bucket;
	max_bucket_p = max_bucket;
	return true;
}

unique_ptr<BucketRewrite> DateBinBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr) {
	auto &children = expr.GetChildren();
	if (children.size() != 3 || expr.GetReturnType().id() != LogicalTypeId::TIMESTAMP ||
	    children[1]->GetReturnType().id() != LogicalTypeId::TIMESTAMP ||
	    children[0]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
	    children[2]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return nullptr;
	}
	const auto &stride_value = children[0]->Cast<BoundConstantExpression>().GetValue();
	const auto &origin_value = children[2]->Cast<BoundConstantExpression>().GetValue();
	if (stride_value.IsNull() || origin_value.IsNull() || stride_value.type().id() != LogicalTypeId::INTERVAL ||
	    origin_value.type().id() != LogicalTypeId::TIMESTAMP) {
		return nullptr;
	}
	const auto stride = stride_value.GetValue<interval_t>();
	const auto origin = origin_value.GetValue<timestamp_t>();
	if (stride.months != 0 || !Value::IsFinite(origin)) {
		return nullptr;
	}
	DateBucketSpec spec;
	spec.width = stride.micros + int64_t(stride.days) * Interval::MICROS_PER_DAY;
	spec.anchor = origin.value;
	if (spec.width <= 0) {
		return nullptr;
	}
	return make_uniq<DateBucketRewrite>(context, spec, 1, LogicalType::TIMESTAMP, LogicalType::TIMESTAMP, false);
}

bool CyclicBucketRewrite::TryBucketRange(const BaseStatistics &input_stats, int64_t &min_bucket_p,
                                         int64_t &max_bucket_p) const {
	min_bucket_p = min_bucket;
	max_bucket_p = max_bucket;
	return true;
}

unique_ptr<Expression> CyclicBucketRewrite::Unbucket(unique_ptr<Expression> bucket) const {
	return MakeUnaryCall(unbucket_function, std::move(bucket));
}

namespace {

struct StrfTimeInspector : public StrfTimeFormat {
	using StrfTimeFormat::specifiers;
};

} // namespace

namespace {

struct TruncationLevel {
	DateCoordinates::Level level;
	DatePartSpecifier part;
	const char *name;
};

const TruncationLevel TRUNCATION_LEVELS[] = {
    {DateCoordinates::Level::MILLENNIUM, DatePartSpecifier::MILLENNIUM, "millennium"},
    {DateCoordinates::Level::CENTURY, DatePartSpecifier::CENTURY, "century"},
    {DateCoordinates::Level::DECADE, DatePartSpecifier::DECADE, "decade"},
    {DateCoordinates::Level::YEAR, DatePartSpecifier::YEAR, "year"},
    {DateCoordinates::Level::QUARTER, DatePartSpecifier::QUARTER, "quarter"},
    {DateCoordinates::Level::MONTH, DatePartSpecifier::MONTH, "month"},
    {DateCoordinates::Level::WEEK, DatePartSpecifier::WEEK, "week"},
    {DateCoordinates::Level::DAY, DatePartSpecifier::DAY, "day"},
    {DateCoordinates::Level::HOUR, DatePartSpecifier::HOUR, "hour"},
    {DateCoordinates::Level::MINUTE, DatePartSpecifier::MINUTE, "minute"},
    {DateCoordinates::Level::SECOND, DatePartSpecifier::SECOND, "second"},
};

} // namespace

void DateCoordinates::Raise(Level level) {
	if (level > finest) {
		finest = level;
	}
}

bool DateCoordinates::AddFormat(const string &format_string) {
	StrfTimeInspector format;
	if (!StrTimeFormat::ParseFormatSpecifier(format_string, format).empty()) {
		return false;
	}
	for (const auto specifier : format.specifiers) {
		switch (specifier) {
		case StrTimeSpecifier::YEAR_DECIMAL:
			year = true;
			Raise(Level::YEAR);
			break;
		case StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED:
		case StrTimeSpecifier::YEAR_WITHOUT_CENTURY:
			year = true;
			two_digit_year = true;
			Raise(Level::YEAR);
			break;
		case StrTimeSpecifier::YEAR_ISO:
			iso_year = true;
			Raise(Level::YEAR);
			break;
		case StrTimeSpecifier::WEEK_NUMBER_ISO:
			iso_week = true;
			Raise(Level::WEEK);
			break;
		case StrTimeSpecifier::ABBREVIATED_MONTH_NAME:
		case StrTimeSpecifier::FULL_MONTH_NAME:
		case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
		case StrTimeSpecifier::MONTH_DECIMAL:
			month = true;
			Raise(Level::MONTH);
			break;
		case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
		case StrTimeSpecifier::DAY_OF_MONTH:
			day = true;
			Raise(Level::DAY);
			break;
		case StrTimeSpecifier::DAY_OF_YEAR_PADDED:
		case StrTimeSpecifier::DAY_OF_YEAR_DECIMAL:
			day_of_year = true;
			Raise(Level::DAY);
			break;
		case StrTimeSpecifier::ABBREVIATED_WEEKDAY_NAME:
		case StrTimeSpecifier::FULL_WEEKDAY_NAME:
		case StrTimeSpecifier::WEEKDAY_DECIMAL:
		case StrTimeSpecifier::WEEKDAY_ISO:
			weekday = true;
			Raise(Level::DAY);
			break;
		case StrTimeSpecifier::HOUR_24_PADDED:
		case StrTimeSpecifier::HOUR_24_DECIMAL:
			hour24 = true;
			Raise(Level::HOUR);
			break;
		case StrTimeSpecifier::HOUR_12_PADDED:
		case StrTimeSpecifier::HOUR_12_DECIMAL:
			hour12 = true;
			Raise(Level::HOUR);
			break;
		case StrTimeSpecifier::AM_PM:
			am_pm = true;
			Raise(Level::HOUR);
			break;
		case StrTimeSpecifier::MINUTE_PADDED:
		case StrTimeSpecifier::MINUTE_DECIMAL:
			minute = true;
			Raise(Level::MINUTE);
			break;
		case StrTimeSpecifier::SECOND_PADDED:
		case StrTimeSpecifier::SECOND_DECIMAL:
			second = true;
			Raise(Level::SECOND);
			break;
		default:
			return false;
		}
	}
	return true;
}

bool DateCoordinates::AddPart(DatePartSpecifier part) {
	switch (part) {
	case DatePartSpecifier::DECADE:
		Raise(Level::DECADE);
		return true;
	case DatePartSpecifier::YEAR:
		year = true;
		Raise(Level::YEAR);
		return true;
	case DatePartSpecifier::ISOYEAR:
		iso_year = true;
		Raise(Level::YEAR);
		return true;
	case DatePartSpecifier::QUARTER:
		quarter = true;
		Raise(Level::QUARTER);
		return true;
	case DatePartSpecifier::MONTH:
		month = true;
		Raise(Level::MONTH);
		return true;
	case DatePartSpecifier::WEEK:
		iso_week = true;
		Raise(Level::WEEK);
		return true;
	case DatePartSpecifier::YEARWEEK:
		iso_year = true;
		iso_week = true;
		Raise(Level::WEEK);
		return true;
	case DatePartSpecifier::DAY:
		day = true;
		Raise(Level::DAY);
		return true;
	case DatePartSpecifier::DOY:
		day_of_year = true;
		Raise(Level::DAY);
		return true;
	case DatePartSpecifier::DOW:
	case DatePartSpecifier::ISODOW:
		weekday = true;
		Raise(Level::DAY);
		return true;
	case DatePartSpecifier::HOUR:
		hour24 = true;
		Raise(Level::HOUR);
		return true;
	case DatePartSpecifier::MINUTE:
		minute = true;
		Raise(Level::MINUTE);
		return true;
	case DatePartSpecifier::SECOND:
		second = true;
		Raise(Level::SECOND);
		return true;
	default:
		return false;
	}
}

bool DateCoordinates::TryResolve(bool sub_day_constant, DatePartSpecifier &part) const {
	auto level = finest;
	if (sub_day_constant && level > Level::DAY) {
		level = Level::DAY;
	}
	if (level == Level::NONE) {
		return false;
	}
	if (level >= Level::YEAR && !year && !iso_year) {
		return false;
	}
	const bool iso = iso_year || iso_week;
	if (iso && (year || quarter || month || day || day_of_year)) {
		return false;
	}
	if (iso && level < Level::DAY && !(iso_year && iso_week && level == Level::WEEK)) {
		return false;
	}
	if (!iso && level == Level::WEEK) {
		return false;
	}
	if (!iso && level >= Level::QUARTER && !quarter && !month && !day_of_year) {
		return false;
	}
	if (!iso && level >= Level::MONTH && !month && !day_of_year) {
		return false;
	}
	if (level >= Level::DAY && !(month && day) && !day_of_year && !(iso_year && iso_week && weekday)) {
		return false;
	}
	if (level >= Level::HOUR && !hour24 && !(hour12 && am_pm)) {
		return false;
	}
	if (level >= Level::MINUTE && !minute) {
		return false;
	}
	if (level >= Level::SECOND && !second) {
		return false;
	}
	auto entry = absl::c_find_if(TRUNCATION_LEVELS, [&](const TruncationLevel &l) { return l.level == level; });
	part = entry != std::end(TRUNCATION_LEVELS) ? entry->part : DatePartSpecifier::SECOND;
	return true;
}

const char *DateTruncPartName(DatePartSpecifier part) {
	auto entry = absl::c_find_if(TRUNCATION_LEVELS, [&](const TruncationLevel &l) { return l.part == part; });
	return entry != std::end(TRUNCATION_LEVELS) ? entry->name : "second";
}

bool DateCoordinates::TimeOfDayOnly() const {
	if (year || iso_year || quarter || month || iso_week || day || day_of_year || weekday) {
		return false;
	}
	return hour24 || (hour12 && am_pm);
}

bool TimeOfDayGrid(int64_t width, int64_t anchor) {
	return width > 0 && Interval::MICROS_PER_DAY % width == 0 && anchor % width == 0;
}

bool TryGetTimeOfDayWidth(const string &format_string, int64_t &width) {
	DateCoordinates coordinates;
	if (!coordinates.AddFormat(format_string) || !coordinates.TimeOfDayOnly()) {
		return false;
	}
	switch (coordinates.finest) {
	case DateCoordinates::Level::HOUR:
		width = Interval::MICROS_PER_HOUR;
		return true;
	case DateCoordinates::Level::MINUTE:
		if (!coordinates.minute) {
			return false;
		}
		width = Interval::MICROS_PER_MINUTE;
		return true;
	case DateCoordinates::Level::SECOND:
		if (!coordinates.minute || !coordinates.second) {
			return false;
		}
		width = Interval::MICROS_PER_SEC;
		return true;
	default:
		return false;
	}
}

bool TryGetStrfTimeGranularity(const string &format_string, bool sub_day_constant, DatePartSpecifier &part,
                               bool &two_digit_year) {
	DateCoordinates coordinates;
	if (!coordinates.AddFormat(format_string)) {
		return false;
	}
	two_digit_year = coordinates.two_digit_year;
	return coordinates.TryResolve(sub_day_constant, part);
}

namespace {

unique_ptr<BucketRewrite> NaiveTimeOfDay(ClientContext &context, const LogicalType &input_type, idx_t input_index,
                                         int64_t width, const Value &format, optional_ptr<Expression> input) {
	const auto functions = InternalTimeOfDayBucketFun::GetFunctions().functions;
	switch (input_type.id()) {
	case LogicalTypeId::TIMESTAMP:
		return TimeOfDayRewrite(context, functions[0], nullptr, input_index, width, format, input);
	case LogicalTypeId::TIMESTAMP_NS:
		return TimeOfDayRewrite(context, functions[1], nullptr, input_index, width, format, input);
	default:
		return nullptr;
	}
}

} // namespace

unique_ptr<BucketRewrite> StrfTimeBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr) {
	auto &children = expr.GetChildren();
	if (children.size() != 2) {
		return nullptr;
	}
	idx_t input_index = 0;
	for (; input_index < 2; input_index++) {
		const auto id = children[input_index]->GetReturnType().id();
		if (id == LogicalTypeId::TIMESTAMP || id == LogicalTypeId::DATE || id == LogicalTypeId::TIMESTAMP_NS) {
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
	bool two_digit_year = false;
	if (!TryGetStrfTimeGranularity(StringValue::Get(format_value), input_type.id() == LogicalTypeId::DATE, part,
	                               two_digit_year) ||
	    !TryGetDateTruncSpec(part, spec)) {
		int64_t width = 0;
		if (!TryGetTimeOfDayWidth(StringValue::Get(format_value), width)) {
			return nullptr;
		}
		return NaiveTimeOfDay(context, input_type, input_index, width, format_value, nullptr);
	}
	auto inner = make_uniq<DateBucketRewrite>(context, spec, input_index, input_type, input_type, false);
	if (two_digit_year) {
		inner->RequireYearSpanBelow(100);
	}
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
		return MakeUnaryCall(bucket_function, std::move(input));
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

namespace {

template <class INPUT>
int64_t TimeOfDaySlot(INPUT ts, int64_t width) {
	if (width <= 0) {
		throw InvalidInputException("Time of day bucket width must be positive");
	}
	const auto micros = Micros(ts);
	const auto day = DateTrunc::FloorDiv(micros, Interval::MICROS_PER_DAY);
	return (micros - day * Interval::MICROS_PER_DAY) / width;
}

template <class INPUT>
void TimeOfDayFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &width_vector = args.data[1];
	if (width_vector.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(width_vector)) {
		const auto width = *ConstantVector::GetData<int64_t>(width_vector);
		UnaryExecutor::Execute<INPUT, int64_t>(args.data[0], result, args.size(),
		                                       [&](INPUT input) { return TimeOfDaySlot(input, width); });
		return;
	}
	BinaryExecutor::Execute<INPUT, int64_t, int64_t>(args.data[0], args.data[1], result, args.size(),
	                                                 TimeOfDaySlot<INPUT>);
}

ScalarFunction TimeOfDayFunctionFor(const LogicalType &input_type, scalar_function_t function) {
	return ScalarFunction(Identifier(InternalTimeOfDayBucketFun::Name), {input_type, LogicalType::BIGINT},
	                      LogicalType::BIGINT, std::move(function));
}

} // namespace

ScalarFunctionSet InternalTimeOfDayBucketFun::GetFunctions() {
	ScalarFunctionSet set(Name);
	set.AddFunction(TimeOfDayFunctionFor(LogicalType::TIMESTAMP, TimeOfDayFunction<timestamp_t>));
	set.AddFunction(TimeOfDayFunctionFor(LogicalType::TIMESTAMP_NS, TimeOfDayFunction<timestamp_ns_t>));
	return set;
}

TimeOfDayBucketRewrite::TimeOfDayBucketRewrite(ClientContext &context_p, ScalarFunction bucket_function_p,
                                               unique_ptr<FunctionData> bind_info_p, idx_t input_index_p,
                                               int64_t width_p, unique_ptr<Expression> shell_p,
                                               unique_ptr<Expression> template_input_p,
                                               optional_ptr<Expression> custom_input_p)
    : context(context_p), bucket_function(std::move(bucket_function_p)), bind_info(std::move(bind_info_p)),
      input_index(input_index_p), width(width_p), shell(std::move(shell_p)),
      template_input(std::move(template_input_p)), custom_input(custom_input_p) {
}

idx_t TimeOfDayBucketRewrite::InputIndex() const {
	return input_index;
}

optional_ptr<Expression> TimeOfDayBucketRewrite::CustomInput() const {
	return custom_input;
}

bool TimeOfDayBucketRewrite::TryBucketRange(const BaseStatistics &input_stats, int64_t &min_bucket,
                                            int64_t &max_bucket) const {
	int64_t min = 0;
	int64_t max = 0;
	bool zoned = false;
	if (!TryGetMicrosRange(input_stats, min, max, zoned)) {
		return false;
	}
	min_bucket = 0;
	max_bucket = (Interval::MICROS_PER_DAY + width - 1) / width - 1;
	return true;
}

unique_ptr<Expression> TimeOfDayBucketRewrite::Bucket(unique_ptr<Expression> input) const {
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(std::move(input));
	arguments.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(width)));
	return MakeBucketCall(bucket_function, std::move(arguments), bind_info ? bind_info->Copy() : nullptr);
}

unique_ptr<Expression> TimeOfDayBucketRewrite::Unbucket(unique_ptr<Expression> bucket) const {
	DateBucketSpec spec;
	spec.width = width;
	auto representative = MakeCall(InternalDateTruncUnbucketFun::GetFunction(), std::move(bucket), spec);
	return RebuildShell(context, *shell, *template_input, std::move(representative));
}

unique_ptr<BucketRewrite> TimeOfDayRewrite(ClientContext &context, ScalarFunction bucket_function,
                                           unique_ptr<FunctionData> bind_info, idx_t input_index, int64_t width,
                                           const Value &format, optional_ptr<Expression> custom_input) {
	unique_ptr<Expression> template_input = make_uniq<BoundConstantExpression>(Value::TIMESTAMP(timestamp_t(0)));
	unique_ptr<Expression> shell;
	if (format.IsNull()) {
		shell = BoundCastExpression::AddCastToType(context, template_input->Copy(), LogicalType::TIME);
	} else {
		vector<unique_ptr<Expression>> arguments;
		arguments.push_back(template_input->Copy());
		arguments.push_back(make_uniq<BoundConstantExpression>(format));
		FunctionBinder binder(context);
		ErrorData error;
		shell =
		    binder.BindScalarFunction(Identifier(DEFAULT_SCHEMA), Identifier("strftime"), std::move(arguments), error);
	}
	if (!shell) {
		return nullptr;
	}
	return make_uniq<TimeOfDayBucketRewrite>(context, std::move(bucket_function), std::move(bind_info), input_index,
	                                         width, std::move(shell), std::move(template_input), custom_input);
}

unique_ptr<BucketRewrite> DateBucketRewrite::TryTimeOfDay(ClientContext &context_p, Expression &input) const {
	if (spec.calendar || !TimeOfDayGrid(spec.width, spec.anchor)) {
		return nullptr;
	}
	return NaiveTimeOfDay(context_p, input_type, 0, spec.width, Value(), &input);
}

unique_ptr<BucketRewrite> LastDayBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr) {
	auto &children = expr.GetChildren();
	if (children.size() != 1) {
		return nullptr;
	}
	const auto &input_type = children[0]->GetReturnType();
	if (input_type.id() != LogicalTypeId::TIMESTAMP && input_type.id() != LogicalTypeId::DATE) {
		return nullptr;
	}
	DateBucketSpec spec;
	TryGetDateTruncSpec(DatePartSpecifier::MONTH, spec);
	auto inner = make_uniq<DateBucketRewrite>(context, spec, 0, input_type, input_type, false);
	return make_uniq<FunctionBucketRewrite>(std::move(inner), expr, 0);
}

ScalarFunctionSet InternalMonthOfYearFun::GetFunctions() {
	ScalarFunctionSet set(Name);
	set.AddFunction(
	    ScalarFunction(Identifier(Name), {LogicalType::DATE}, LogicalType::BIGINT, MonthOfYearFunction<date_t>));
	set.AddFunction(ScalarFunction(Identifier(Name), {LogicalType::TIMESTAMP}, LogicalType::BIGINT,
	                               MonthOfYearFunction<timestamp_t>));
	return set;
}

ScalarFunctionSet InternalDayOfWeekFun::GetFunctions() {
	ScalarFunctionSet set(Name);
	set.AddFunction(
	    ScalarFunction(Identifier(Name), {LogicalType::DATE}, LogicalType::BIGINT, DayOfWeekFunction<date_t>));
	set.AddFunction(ScalarFunction(Identifier(Name), {LogicalType::TIMESTAMP}, LogicalType::BIGINT,
	                               DayOfWeekFunction<timestamp_t>));
	return set;
}

ScalarFunction InternalMonthNameFun::GetFunction() {
	return ScalarFunction(Identifier(Name), {LogicalType::BIGINT}, LogicalType::VARCHAR, MonthNameFunction);
}

ScalarFunction InternalDayNameFun::GetFunction() {
	return ScalarFunction(Identifier(Name), {LogicalType::BIGINT}, LogicalType::VARCHAR, DayNameFunction);
}

bool DateBucketSpec::TryBucket(int64_t micros, int64_t &result) const {
	const auto value = calendar ? DateTrunc::MonthIndex(timestamp_t(micros)) : micros;
	int64_t offset = 0;
	if (!TrySubtractOperator::Operation(value, anchor, offset)) {
		return false;
	}
	result = DateTrunc::FloorDiv(offset, width);
	return true;
}

DateBucketRewrite::DateBucketRewrite(ClientContext &context_p, DateBucketSpec spec_p, idx_t input_index_p,
                                     LogicalType input_type_p, LogicalType result_type_p, bool anno_domini_only_p)
    : context(context_p), spec(spec_p), input_index(input_index_p), input_type(std::move(input_type_p)),
      result_type(std::move(result_type_p)), anno_domini_only(anno_domini_only_p) {
}

idx_t DateBucketRewrite::InputIndex() const {
	return input_index;
}

bool DateBucketRewrite::Contains(const GranularBucketRewrite &finer) const {
	auto core = finer.Core();
	auto other = core ? dynamic_cast<const DateBucketRewrite *>(core.get()) : nullptr;
	if (!other || other->input_type != input_type) {
		return false;
	}
	if (spec.calendar == other->spec.calendar) {
		return Nested(spec.width, spec.anchor, other->spec.width, other->spec.anchor);
	}
	if (spec.calendar) {
		return other->spec.width > 0 && Interval::MICROS_PER_DAY % other->spec.width == 0 &&
		       other->spec.anchor % other->spec.width == 0;
	}
	return false;
}

int64_t DateBucketRewrite::GranularityMicros() const {
	if (spec.calendar || result_type.id() == LogicalTypeId::DATE) {
		return Interval::MICROS_PER_DAY;
	}
	return std::gcd(spec.width, spec.anchor);
}

bool TryGetMicrosRange(const BaseStatistics &stats, int64_t &min, int64_t &max, bool &zoned) {
	if (stats.GetStatsType() != StatisticsType::NUMERIC_STATS || !NumericStats::HasMinMax(stats)) {
		return false;
	}
	const auto limit = NumericLimits<int64_t>::Maximum() - 2 * Interval::MICROS_PER_WEEK;
	zoned = stats.GetType().id() == LogicalTypeId::TIMESTAMP_TZ;
	switch (stats.GetType().id()) {
	case LogicalTypeId::TIMESTAMP:
		if (!TryScaledRange<timestamp_t>(stats, 1, limit, min, max)) {
			return false;
		}
		break;
	case LogicalTypeId::TIMESTAMP_TZ:
		if (!TryScaledRange<timestamp_tz_t>(stats, 1, limit, min, max)) {
			return false;
		}
		break;
	case LogicalTypeId::DATE:
		if (!TryScaledRange<date_t>(stats, Interval::MICROS_PER_DAY, limit, min, max)) {
			return false;
		}
		break;
	case LogicalTypeId::TIMESTAMP_MS:
		if (!TryScaledRange<timestamp_ms_t>(stats, Interval::MICROS_PER_MSEC, limit, min, max)) {
			return false;
		}
		break;
	case LogicalTypeId::TIMESTAMP_SEC:
		if (!TryScaledRange<timestamp_sec_t>(stats, Interval::MICROS_PER_SEC, limit, min, max)) {
			return false;
		}
		break;
	case LogicalTypeId::TIME:
		if (!TryScaledRange<dtime_t>(stats, 1, limit, min, max)) {
			return false;
		}
		break;
	case LogicalTypeId::TIMESTAMP_NS: {
		const auto lo = NumericStats::GetMin<timestamp_ns_t>(stats);
		const auto hi = NumericStats::GetMax<timestamp_ns_t>(stats);
		if (!Finite(lo) || !Finite(hi)) {
			return false;
		}
		min = DateTrunc::FloorDiv(lo.value, Interval::NANOS_PER_MICRO);
		max = -DateTrunc::FloorDiv(-hi.value, Interval::NANOS_PER_MICRO);
		break;
	}
	default:
		return false;
	}
	return min <= max && min >= -limit && max <= limit;
}

bool DateBucketRewrite::TryBucketRange(const BaseStatistics &stats, int64_t &min_bucket, int64_t &max_bucket) const {
	int64_t min = 0;
	int64_t max = 0;
	bool zoned = false;
	if (!TryGetMicrosRange(stats, min, max, zoned)) {
		return false;
	}
	if (zoned != (input_type.id() == LogicalTypeId::TIMESTAMP_TZ)) {
		min -= Interval::MICROS_PER_DAY;
		max += Interval::MICROS_PER_DAY;
	}
	const auto limit = NumericLimits<int64_t>::Maximum() - Interval::MICROS_PER_WEEK;
	const auto lower = anno_domini_only && spec.calendar ? DateTrunc::YearStart(1) * Interval::MICROS_PER_DAY : -limit;
	if (min < lower || max > limit) {
		return false;
	}
	if (max_year_span && DateTrunc::ToYearDay(DateTrunc::ToDays(timestamp_t(max))).year -
	                             DateTrunc::ToYearDay(DateTrunc::ToDays(timestamp_t(min))).year >=
	                         max_year_span) {
		return false;
	}
	return spec.TryBucket(min, min_bucket) && spec.TryBucket(max, max_bucket);
}

unique_ptr<Expression> DateBucketRewrite::Bucket(unique_ptr<Expression> input) const {
	auto type = input_type.id();
	if (type != LogicalTypeId::TIMESTAMP && type != LogicalTypeId::TIMESTAMP_TZ &&
	    type != LogicalTypeId::TIMESTAMP_NS && type != LogicalTypeId::TIME) {
		input = BoundCastExpression::AddCastToType(context, std::move(input), LogicalType::TIMESTAMP);
		type = LogicalTypeId::TIMESTAMP;
	}
	const auto set =
	    spec.calendar ? InternalDateTruncMonthBucketFun::GetFunctions() : InternalDateTruncBucketFun::GetFunctions();
	return MakeCall(FunctionFor(set, type), std::move(input), spec);
}

unique_ptr<Expression> DateBucketRewrite::Unbucket(unique_ptr<Expression> bucket) const {
	if (result_type.id() == LogicalTypeId::TIME) {
		return MakeCall(InternalDateTruncUnbucketTimeFun::GetFunction(), std::move(bucket), spec);
	}
	const bool zoned = result_type.id() == LogicalTypeId::TIMESTAMP_TZ;
	ScalarFunction function = spec.calendar ? (zoned ? InternalDateTruncMonthUnbucketTzFun::GetFunction()
	                                                 : InternalDateTruncMonthUnbucketFun::GetFunction())
	                                        : (zoned ? InternalDateTruncUnbucketTzFun::GetFunction()
	                                                 : InternalDateTruncUnbucketFun::GetFunction());
	auto expr = MakeCall(function, std::move(bucket), spec);
	if (result_type.id() != LogicalTypeId::TIMESTAMP && result_type.id() != LogicalTypeId::TIMESTAMP_TZ) {
		expr = BoundCastExpression::AddCastToType(context, std::move(expr), result_type);
	}
	return expr;
}

ScalarFunctionSet InternalDateTruncBucketFun::GetFunctions() {
	ScalarFunctionSet set(Name);
	set.AddFunction(
	    BucketFunction<timestamp_t, int64_t, Bucket<timestamp_t>>(Name, LogicalType::TIMESTAMP, LogicalType::BIGINT));
	set.AddFunction(BucketFunction<timestamp_tz_t, int64_t, Bucket<timestamp_tz_t>>(Name, LogicalType::TIMESTAMP_TZ,
	                                                                                LogicalType::BIGINT));
	set.AddFunction(BucketFunction<timestamp_ns_t, int64_t, Bucket<timestamp_ns_t>>(Name, LogicalType::TIMESTAMP_NS,
	                                                                                LogicalType::BIGINT));
	set.AddFunction(BucketFunction<dtime_t, int64_t, Bucket<dtime_t>>(Name, LogicalType::TIME, LogicalType::BIGINT));
	return set;
}

ScalarFunction InternalDateTruncUnbucketTimeFun::GetFunction() {
	return BucketFunction<int64_t, dtime_t, Unbucket<dtime_t>>(Name, LogicalType::BIGINT, LogicalType::TIME);
}

ScalarFunctionSet InternalDateTruncMonthBucketFun::GetFunctions() {
	ScalarFunctionSet set(Name);
	set.AddFunction(BucketFunction<timestamp_t, int64_t, MonthBucket<timestamp_t>>(Name, LogicalType::TIMESTAMP,
	                                                                               LogicalType::BIGINT));
	set.AddFunction(BucketFunction<timestamp_tz_t, int64_t, MonthBucket<timestamp_tz_t>>(
	    Name, LogicalType::TIMESTAMP_TZ, LogicalType::BIGINT));
	set.AddFunction(BucketFunction<timestamp_ns_t, int64_t, MonthBucket<timestamp_ns_t>>(
	    Name, LogicalType::TIMESTAMP_NS, LogicalType::BIGINT));
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
