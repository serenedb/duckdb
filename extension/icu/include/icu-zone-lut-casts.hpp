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

#include "duckdb/common/operator/date_trunc_operators.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
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
		result = timestamp_ns_t(cast.value * Interval::NANOS_PER_MICRO + nanos);
		return true;
	}

	static inline bool Try(const ZoneLUT &lut, timestamp_tz_t instant, dtime_tz_t &result) {
		int64_t offset = 0;
		if (!instant.IsFinite() || !lut.TryOffset(instant.value, offset)) {
			return false;
		}
		const int64_t wall = instant.value + offset;
		const int64_t micros = wall - DateTrunc::FloorDiv(wall, Interval::MICROS_PER_DAY) * Interval::MICROS_PER_DAY;
		result = dtime_tz_t(dtime_t(micros), UnsafeNumericCast<int32_t>(offset / Interval::MICROS_PER_SEC));
		return true;
	}

	static inline bool Try(const ZoneLUT &lut, timestamp_tz_t instant, date_t &result) {
		int64_t offset = 0;
		if (!instant.IsFinite() || !lut.TryOffset(instant.value, offset)) {
			return false;
		}
		result = date_t(UnsafeNumericCast<int32_t>(DateTrunc::FloorDiv(instant.value + offset, Interval::MICROS_PER_DAY)));
		return true;
	}

	template <class SRC, class DST>
	static inline bool Try(const ZoneLUT &lut, SRC input, DST &result) {
		return false;
	}

	template <class SRC, class DST>
	static bool TryCast(const ZoneLUT *lut, Vector &source, Vector &result, idx_t count) {
		if (!lut) {
			return false;
		}
		bool covered = true;
		UnaryExecutor::Execute<SRC, DST>(source, result, count, [&](SRC input) {
			DST converted;
			if (!Try(*lut, input, converted)) {
				covered = false;
				return DST();
			}
			return converted;
		});
		return covered;
	}
};

} // namespace duckdb
