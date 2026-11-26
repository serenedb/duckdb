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

#include "duckdb/common/limits.hpp"
#include "duckdb/common/operator/coarsen_timestamp.hpp"
#include "duckdb/common/operator/date_trunc_operators.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "icu-zone-lut.hpp"

namespace duckdb {

struct ICUZoneCasts {
	static inline bool Try(const ZoneLUT &lut, timestamp_t naive, timestamp_tz_t &result) {
		if (!naive.IsFinite()) {
			result = timestamp_tz_t(naive);
			return true;
		}
		return lut.TryResolve(naive.value, result.value);
	}

	static inline bool Try(const ZoneLUT &lut, timestamp_ns_t naive, timestamp_tz_ns_t &result) {
		if (!naive.IsFinite()) {
			result = timestamp_tz_ns_t(naive);
			return true;
		}
		const auto nanos = naive.value % Interval::NANOS_PER_MICRO;
		timestamp_tz_t cast;
		if (!Try(lut, timestamp_t(naive.value / Interval::NANOS_PER_MICRO), cast)) {
			return false;
		}
		timestamp_ns_t ns;
		if (!Timestamp::TryFromTimestampNanos(timestamp_t(cast), nanos, ns)) {
			return false;
		}
		result = timestamp_tz_ns_t(ns);
		return true;
	}

	static inline bool Try(const ZoneLUT &lut, timestamp_tz_t instant, timestamp_t &result) {
		if (!instant.IsFinite()) {
			result = timestamp_t(instant);
			return true;
		}
		int64_t offset = 0;
		if (!lut.TryOffset(instant.value, offset)) {
			return false;
		}
		result = timestamp_t(instant.value + offset);
		return true;
	}

	static inline bool Try(const ZoneLUT &lut, timestamp_tz_ns_t instant, timestamp_ns_t &result) {
		if (!instant.IsFinite()) {
			result = timestamp_ns_t(instant);
			return true;
		}
		const auto nanos = instant.value % Interval::NANOS_PER_MICRO;
		timestamp_t cast;
		if (!Try(lut, timestamp_tz_t(instant.value / Interval::NANOS_PER_MICRO), cast)) {
			return false;
		}
		return Timestamp::TryFromTimestampNanos(cast, UnsafeNumericCast<int32_t>(nanos), result);
	}

	static inline bool Try(const ZoneLUT &lut, timestamp_tz_t instant, dtime_tz_t &result) {
		int64_t offset = 0;
		if (!instant.IsFinite() || !lut.TryOffset(instant.value, offset)) {
			return false;
		}
		const int64_t offset_seconds = offset / Interval::MICROS_PER_SEC;
		if (offset_seconds < dtime_tz_t::MIN_OFFSET || offset_seconds > dtime_tz_t::MAX_OFFSET) {
			return false;
		}
		const int64_t wall = instant.value + offset;
		const int64_t micros = wall - DateTrunc::FloorDiv(wall, Interval::MICROS_PER_DAY) * Interval::MICROS_PER_DAY;
		result = dtime_tz_t(dtime_t(micros), UnsafeNumericCast<int32_t>(offset_seconds));
		return true;
	}

	static inline bool Try(const ZoneLUT &lut, timestamp_tz_t instant, date_t &result) {
		int64_t offset = 0;
		if (!instant.IsFinite() || !lut.TryOffset(instant.value, offset)) {
			return false;
		}
		result =
		    date_t(UnsafeNumericCast<int32_t>(DateTrunc::FloorDiv(instant.value + offset, Interval::MICROS_PER_DAY)));
		return true;
	}

	template <class SRC, class DST>
	static inline bool Try(const ZoneLUT &lut, SRC input, DST &result) {
		return false;
	}

	template <class SRC, class DST>
	static inline DST Shift(int64_t value, int64_t shift) {
		using NAIVE = timestamp_base_t<SRC::PRECISION, false>;
		const NAIVE naive(value + shift);
		if constexpr (std::is_same<NAIVE, DST>::value) {
			return naive;
		} else {
			return CoarsenTimestamp<NAIVE, DST>(naive);
		}
	}

	template <class SRC>
	static inline int64_t FixedShift(const ZoneLUT &lut) {
		return lut.FixedOffset() * (SRC::PRECISION / Interval::MICROS_PER_SEC);
	}

	template <class SRC, class DST>
	static unique_ptr<BaseStatistics> FixedShiftStatistics(const ZoneLUT &lut, const BaseStatistics &stats,
	                                                       const LogicalType &target) {
		if (!lut.HasFixedOffset() || stats.GetStatsType() != StatisticsType::NUMERIC_STATS ||
		    !NumericStats::HasMinMax(stats)) {
			return nullptr;
		}
		const auto shift = FixedShift<SRC>(lut);
		const auto limit = NumericLimits<int64_t>::Maximum() - 1;
		const auto min = NumericStats::GetMin<int64_t>(stats);
		const auto max = NumericStats::GetMax<int64_t>(stats);
		if (min < -limit - MinValue<int64_t>(shift, 0) || max > limit - MaxValue<int64_t>(shift, 0)) {
			return nullptr;
		}
		auto result = NumericStats::CreateEmpty(target);
		result.CopyBase(stats);
		NumericStats::SetMin(result, Value::CreateValue(Shift<SRC, DST>(min, shift)));
		NumericStats::SetMax(result, Value::CreateValue(Shift<SRC, DST>(max, shift)));
		return result.ToUnique();
	}

	template <class SRC, class DST>
	static void FixedShiftCast(const ZoneLUT &lut, Vector &source, Vector &result, idx_t count) {
		const auto shift = FixedShift<SRC>(lut);
		UnaryExecutor::Execute<SRC, DST>(source, result, count,
		                                 [shift](SRC input) { return Shift<SRC, DST>(input.value, shift); });
	}

	template <class SRC, class DST, class FALLBACK>
	static bool TryCast(const ZoneLUT *lut, Vector &source, Vector &result, idx_t count, FALLBACK &&outside) {
		if (!lut) {
			return false;
		}
		UnaryExecutor::Execute<SRC, DST>(source, result, count, [&](SRC input) {
			DST converted;
			return Try(*lut, input, converted) ? converted : outside(input);
		});
		return true;
	}
};

} // namespace duckdb
