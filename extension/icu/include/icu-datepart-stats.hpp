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

#pragma once

#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/operator/date_trunc_operators.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "icu-zone-lut.hpp"

namespace duckdb {

struct ICUDatePartStats {
	static constexpr int64_t MAX_OFFSET_SECONDS = Interval::SECS_PER_DAY;

	static bool TryGetPart(FunctionStatisticsInput &input, DatePartSpecifier &part) {
		auto &children = input.expr.GetChildren();
		if (children.size() == 2) {
			if (children[0]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
				return false;
			}
			const auto &value = children[0]->Cast<BoundConstantExpression>().GetValue();
			return !value.IsNull() && TryGetDatePartSpecifier(StringValue::Get(value), part);
		}
		return children.size() == 1 &&
		       TryGetDatePartSpecifier(input.expr.Function().GetName().GetIdentifierName(), part);
	}

	static unique_ptr<BaseStatistics> Make(const BaseStatistics &child, const LogicalType &type, const Value &min,
	                                       const Value &max) {
		auto result = NumericStats::CreateEmpty(type);
		result.CopyValidity(child);
		NumericStats::SetMin(result, min);
		NumericStats::SetMax(result, max);
		return result.ToUnique();
	}

	static unique_ptr<BaseStatistics> Bounded(const BaseStatistics &child, const LogicalType &type, int64_t min,
	                                          int64_t max) {
		if (type.id() != LogicalTypeId::BIGINT) {
			return nullptr;
		}
		return Make(child, type, Value::BIGINT(min), Value::BIGINT(max));
	}

	static int64_t YearOf(int64_t micros) {
		return DateTrunc::ToYearDay(DateTrunc::ToDays(timestamp_t(micros))).year;
	}

	static int64_t Century(int64_t year) {
		return ((year - 1) / 100) + 1;
	}

	static int64_t Millennium(int64_t year) {
		return ((year - 1) / 1000) + 1;
	}

	static unique_ptr<BaseStatistics> Propagate(ClientContext &context, FunctionStatisticsInput &input) {
		DatePartSpecifier part;
		if (!TryGetPart(input, part) || input.child_stats.empty()) {
			return nullptr;
		}
		auto &child = input.child_stats.back();
		if (child.GetType().id() != LogicalTypeId::TIMESTAMP_TZ || !NumericStats::HasMinMax(child)) {
			return nullptr;
		}
		const auto min = NumericStats::GetMin<timestamp_tz_t>(child);
		const auto max = NumericStats::GetMax<timestamp_tz_t>(child);
		if (min > max || !min.IsFinite() || !max.IsFinite()) {
			return nullptr;
		}
		const auto &type = input.expr.GetReturnType();
		switch (part) {
		case DatePartSpecifier::MONTH:
			return Bounded(child, type, 1, 12);
		case DatePartSpecifier::DAY:
			return Bounded(child, type, 1, 31);
		case DatePartSpecifier::QUARTER:
			return Bounded(child, type, 1, 4);
		case DatePartSpecifier::DOW:
			return Bounded(child, type, 0, 6);
		case DatePartSpecifier::ISODOW:
			return Bounded(child, type, 1, 7);
		case DatePartSpecifier::DOY:
			return Bounded(child, type, 1, 366);
		case DatePartSpecifier::WEEK:
			return Bounded(child, type, 1, 53);
		case DatePartSpecifier::MICROSECONDS:
			return Bounded(child, type, 0, 59999999);
		case DatePartSpecifier::MILLISECONDS:
			return Bounded(child, type, 0, 59999);
		case DatePartSpecifier::SECOND:
		case DatePartSpecifier::MINUTE:
			return Bounded(child, type, 0, 59);
		case DatePartSpecifier::HOUR:
			return Bounded(child, type, 0, 24);
		case DatePartSpecifier::ERA:
			return Bounded(child, type, 0, 1);
		case DatePartSpecifier::TIMEZONE:
			return Bounded(child, type, -MAX_OFFSET_SECONDS, MAX_OFFSET_SECONDS);
		case DatePartSpecifier::TIMEZONE_HOUR:
			return Bounded(child, type, -MAX_OFFSET_SECONDS / Interval::SECS_PER_HOUR,
			               MAX_OFFSET_SECONDS / Interval::SECS_PER_HOUR);
		case DatePartSpecifier::TIMEZONE_MINUTE:
			return Bounded(child, type, -59, 59);
		case DatePartSpecifier::EPOCH:
			if (type.id() != LogicalTypeId::DOUBLE) {
				return nullptr;
			}
			return Make(child, type, Value::DOUBLE(double(min.value) / Interval::MICROS_PER_SEC),
			            Value::DOUBLE(double(max.value) / Interval::MICROS_PER_SEC));
		default:
			break;
		}
		const auto margin = MAX_OFFSET_SECONDS * Interval::MICROS_PER_SEC;
		if (min.value < ZoneLUT::FIRST_ANNO_DOMINI + margin || max.value > NumericLimits<int64_t>::Maximum() - margin) {
			return nullptr;
		}
		const auto low = YearOf(min.value - margin);
		const auto high = YearOf(max.value + margin);
		switch (part) {
		case DatePartSpecifier::YEAR:
			return Bounded(child, type, low, high);
		case DatePartSpecifier::ISOYEAR:
			return Bounded(child, type, low - 1, high + 1);
		case DatePartSpecifier::DECADE:
			return Bounded(child, type, low / 10, high / 10);
		case DatePartSpecifier::CENTURY:
			return Bounded(child, type, Century(low), Century(high));
		case DatePartSpecifier::MILLENNIUM:
			return Bounded(child, type, Millennium(low), Millennium(high));
		default:
			return nullptr;
		}
	}
};

} // namespace duckdb
