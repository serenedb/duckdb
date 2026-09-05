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
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "icu-constant-args.hpp"
#include "icu-datefunc.hpp"
#include "icu-zone-lut.hpp"

#include <type_traits>

namespace duckdb {

template <class OP>
struct ICUDateTruncRecomputes {
	using type = void;
};

template <>
struct ICUDateTruncRecomputes<DateTrunc::QuarterOperator> {
	using type = DateTrunc::MonthOperator;
};

template <>
struct ICUDateTruncRecomputes<DateTrunc::DecadeOperator> {
	using type = DateTrunc::YearOperator;
};

template <>
struct ICUDateTruncRecomputes<DateTrunc::CenturyOperator> {
	using type = DateTrunc::YearOperator;
};

template <>
struct ICUDateTruncRecomputes<DateTrunc::MillenniumOperator> {
	using type = DateTrunc::YearOperator;
};

struct ICUDateTruncLUT {
	template <class OP>
	static constexpr bool UsesEraYear() {
		return std::is_same<OP, DateTrunc::DecadeOperator>::value || std::is_same<OP, DateTrunc::CenturyOperator>::value ||
		       std::is_same<OP, DateTrunc::MillenniumOperator>::value;
	}

	template <class OP>
	static constexpr bool PreservesOffset() {
		return std::is_same<OP, DateTrunc::MinuteOperator>::value ||
		       std::is_same<OP, DateTrunc::SecondOperator>::value ||
		       std::is_same<OP, DateTrunc::MillisecondOperator>::value ||
		       std::is_same<OP, DateTrunc::MicrosecondOperator>::value;
	}

	template <class OP, class = void>
	struct TruncatesDays : std::false_type {};

	template <class OP>
	struct TruncatesDays<OP, std::void_t<decltype(OP::Days(int32_t()))>> : std::true_type {};

	[[gnu::always_inline]] static inline bool InGap(const ZoneLUT &lut, int64_t days) {
		const int64_t wall = days * Interval::MICROS_PER_DAY;
		int64_t instant = 0;
		int64_t instant_day = 0;
		int64_t offset = 0;
		if (!lut.TryResolveDay(days - ZoneLUT::FIRST_DAY, wall, instant) ||
		    !lut.TryInstantDay(instant, instant_day, offset)) {
			return true;
		}
		return instant + offset != wall;
	}

	template <class OP>
	[[gnu::always_inline]] static inline bool TryTruncate(const ZoneLUT &lut, timestamp_tz_t input,
	                                                      timestamp_tz_t &result) {
		int64_t offset = 0;
		int64_t instant_day = 0;
		if (!lut.TryInstantDay(input.value, instant_day, offset)) {
			return false;
		}
		const timestamp_t wall(input.value + offset);
		if constexpr (UsesEraYear<OP>()) {
			if (wall.value < ZoneLUT::FIRST_ANNO_DOMINI) {
				return false;
			}
		}
		if constexpr (TruncatesDays<OP>::value) {
			const auto days = DateTrunc::ToDays(wall);
			using INTERMEDIATE = typename ICUDateTruncRecomputes<OP>::type;
			if constexpr (!std::is_void<INTERMEDIATE>::value) {
				if (InGap(lut, INTERMEDIATE::Days(days))) {
					return false;
				}
			}
			const auto truncated_days = OP::Days(days);
			return lut.TryResolveDay(truncated_days - ZoneLUT::FIRST_DAY, truncated_days * Interval::MICROS_PER_DAY,
			                         result.value);
		} else {
			const auto truncated = OP::template Operation<timestamp_t, timestamp_t>(wall);
			if constexpr (PreservesOffset<OP>()) {
				return lut.TryShiftBack(truncated.value, offset, result.value);
			}
			if (lut.HasFixedOffset()) {
				return lut.TryResolve(truncated.value, result.value);
			}
			const auto start = ZoneLUT::DayStart(instant_day);
			const auto wall_day = instant_day + (wall.value >= start + Interval::MICROS_PER_DAY) - (wall.value < start);
			return lut.TryResolveDay(wall_day, truncated.value, result.value);
		}
	}

	static timestamp_tz_t TruncateWithICU(const ICUDateFunc::BindData &info, CalendarPtr &calendar,
	                                      DatePartSpecifier part, timestamp_tz_t input) {
		if (!calendar) {
			calendar.reset(info.calendar->clone());
		}
		auto truncator = ICUDateFunc::TruncationFactory(part);
		auto micros = ICUDateFunc::SetTime(calendar.get(), input);
		truncator(calendar.get(), micros);
		return ICUDateFunc::GetTimeUnsafe(calendar.get(), micros);
	}

	static timestamp_tz_t Truncate(const ICUDateFunc::BindData &info, CalendarPtr &calendar, DatePartSpecifier part,
	                               timestamp_tz_t input) {
		if (!input.IsFinite()) {
			return input;
		}
		timestamp_tz_t truncated;
		if (info.lut && part != DatePartSpecifier::ERA &&
		    DateTrunc::Dispatch(part, [&](auto op) { return TryTruncate<decltype(op)>(*info.lut, input, truncated); })) {
			return truncated;
		}
		return TruncateWithICU(info, calendar, part, input);
	}

	template <class T>
	static bool TryExecute(DataChunk &args, ExpressionState &state, Vector &result) {
		if (!std::is_same<T, timestamp_tz_t>::value) {
			return false;
		}
		DatePartSpecifier part;
		if (!ICUConstantArgs::TryGetPart(args.data[0], part) || part == DatePartSpecifier::ERA) {
			return false;
		}
		auto &info = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<ICUDateFunc::BindData>();
		if (!info.lut || !info.lut->IsValid()) {
			return false;
		}
		const auto &lut = *info.lut;
		CalendarPtr calendar;
		DateTrunc::Dispatch(part, [&](auto op) {
			UnaryExecutor::Execute<timestamp_tz_t, timestamp_tz_t>(
			    args.data[1], result, args.size(), [&](timestamp_tz_t input) {
				    timestamp_tz_t truncated;
				    if (!input.IsFinite()) {
					    return input;
				    }
				    if (TryTruncate<decltype(op)>(lut, input, truncated)) {
					    return truncated;
				    }
				    return TruncateWithICU(info, calendar, part, input);
			    });
		});
		return true;
	}
};

} // namespace duckdb
