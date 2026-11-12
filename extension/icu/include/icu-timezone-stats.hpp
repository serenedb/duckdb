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

#include "duckdb/common/types/interval.hpp"
#include "duckdb/function/scalar/date_bucket_rewrite.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

namespace duckdb {

struct ICUTimeZoneStats {
	static bool TryGetMicros(const BaseStatistics &stats, int64_t &min, int64_t &max) {
		const auto id = stats.GetType().id();
		if (id != LogicalTypeId::TIMESTAMP && id != LogicalTypeId::TIMESTAMP_TZ) {
			return false;
		}
		bool zoned = false;
		return TryGetMicrosRange(stats, min, max, zoned);
	}

	static unique_ptr<BaseStatistics> Propagate(ClientContext &context, FunctionStatisticsInput &input) {
		if (input.child_stats.empty()) {
			return nullptr;
		}
		auto &child = input.child_stats.back();
		int64_t min = 0;
		int64_t max = 0;
		const auto limit = NumericLimits<int64_t>::Maximum() - 2 * Interval::MICROS_PER_DAY;
		if (!TryGetMicros(child, min, max) || min > max || min < -limit || max > limit) {
			return nullptr;
		}
		min -= Interval::MICROS_PER_DAY;
		max += Interval::MICROS_PER_DAY;
		const auto &type = input.expr.GetReturnType();
		auto result = NumericStats::CreateEmpty(type);
		result.CopyBase(child);
		if (type.id() == LogicalTypeId::TIMESTAMP_TZ) {
			NumericStats::SetMin(result, Value::TIMESTAMPTZ(timestamp_tz_t(min)));
			NumericStats::SetMax(result, Value::TIMESTAMPTZ(timestamp_tz_t(max)));
		} else if (type.id() == LogicalTypeId::TIMESTAMP) {
			NumericStats::SetMin(result, Value::TIMESTAMP(timestamp_t(min)));
			NumericStats::SetMax(result, Value::TIMESTAMP(timestamp_t(max)));
		} else {
			return nullptr;
		}
		return result.ToUnique();
	}
};

} // namespace duckdb
