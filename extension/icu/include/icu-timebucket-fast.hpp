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

#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/date_trunc_operators.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "icu-zone-lut.hpp"

namespace duckdb {

struct ICUTimeBucketFast {
	static constexpr int64_t DEFAULT_ORIGIN_MICROS_1 = 10959 * Interval::MICROS_PER_DAY;
	static constexpr int64_t DEFAULT_ORIGIN_MICROS_2 = 10957 * Interval::MICROS_PER_DAY;
	static constexpr int64_t LAST_ARITHMETIC_MICROS = 2932896LL * Interval::MICROS_PER_DAY;

	enum class Kind : uint8_t { MICROS, DAYS, MONTHS, NONE };

	static Kind Classify(interval_t width) {
		if (width.months == 0 && width.days == 0 && width.micros > 0) {
			return Kind::MICROS;
		}
		if (width.months == 0 && width.days > 0 && width.micros == 0) {
			return Kind::DAYS;
		}
		if (width.months > 0 && width.days == 0 && width.micros == 0) {
			return Kind::MONTHS;
		}
		return Kind::NONE;
	}

	static inline bool InRange(int64_t micros) {
		return micros >= ZoneLUT::FIRST_ANNO_DOMINI && micros <= LAST_ARITHMETIC_MICROS;
	}

	static inline bool TryFixed(int64_t width, int64_t ts, int64_t origin, int64_t &bucket) {
		if (!InRange(ts) || !InRange(origin)) {
			return false;
		}
		bucket = origin + DateTrunc::FloorDiv(ts - origin, width) * width;
		return true;
	}

	static inline bool TryMonths(int32_t width, int64_t ts, int64_t origin, int64_t &bucket) {
		if (!InRange(ts) || !InRange(origin)) {
			return false;
		}
		const auto origin_month = DateTrunc::MonthIndex(timestamp_t(origin));
		const auto months =
		    DateTrunc::FloorDiv(DateTrunc::MonthIndex(timestamp_t(ts)) - origin_month, int64_t(width)) * width +
		    origin_month;
		bucket = DateTrunc::MonthIndexStart(months).value;
		return true;
	}

	static inline bool TryBucket(Kind kind, interval_t width, int64_t ts, int64_t origin, int64_t &bucket) {
		switch (kind) {
		case Kind::MICROS:
			return TryFixed(width.micros, ts, origin, bucket);
		case Kind::DAYS:
			return TryFixed(int64_t(width.days) * Interval::MICROS_PER_DAY, ts, origin, bucket);
		case Kind::MONTHS:
			return TryMonths(width.months, ts, origin, bucket);
		default:
			return false;
		}
	}

	static bool TryGetConstantWidth(Vector &width_arg, interval_t &width, Kind &kind) {
		if (width_arg.GetVectorType() != VectorType::CONSTANT_VECTOR || ConstantVector::IsNull(width_arg)) {
			return false;
		}
		width = *ConstantVector::GetData<interval_t>(width_arg);
		kind = Classify(width);
		return kind != Kind::NONE;
	}

	template <class FUN>
	static bool Execute(Vector &ts_arg, Vector &result, idx_t count, FUN &&fun) {
		bool covered = true;
		UnaryExecutor::Execute<timestamp_tz_t, timestamp_tz_t>(ts_arg, result, count, [&](timestamp_tz_t ts) {
			if (!ts.IsFinite()) {
				return ts;
			}
			int64_t bucket = 0;
			if (!fun(ts.value, bucket)) {
				covered = false;
				return ts;
			}
			return timestamp_tz_t(bucket);
		});
		return covered;
	}

	static bool Dispatch(Kind kind, interval_t width, int64_t origin, Vector &ts_arg, Vector &result, idx_t count) {
		if (!InRange(origin)) {
			return false;
		}
		switch (kind) {
		case Kind::MICROS:
		case Kind::DAYS: {
			const int64_t w = kind == Kind::MICROS ? width.micros : int64_t(width.days) * Interval::MICROS_PER_DAY;
			return Execute(ts_arg, result, count, [&](int64_t ts, int64_t &bucket) {
				if (!InRange(ts)) {
					return false;
				}
				bucket = origin + DateTrunc::FloorDiv(ts - origin, w) * w;
				return true;
			});
		}
		case Kind::MONTHS: {
			const auto origin_month = DateTrunc::MonthIndex(timestamp_t(origin));
			const int64_t w = width.months;
			return Execute(ts_arg, result, count, [&](int64_t ts, int64_t &bucket) {
				if (!InRange(ts)) {
					return false;
				}
				const auto months =
				    DateTrunc::FloorDiv(DateTrunc::MonthIndex(timestamp_t(ts)) - origin_month, w) * w + origin_month;
				bucket = DateTrunc::MonthIndexStart(months).value;
				return true;
			});
		}
		default:
			return false;
		}
	}

	static bool TryBinary(DataChunk &args, Vector &result) {
		interval_t width;
		Kind kind;
		if (!TryGetConstantWidth(args.data[0], width, kind)) {
			return false;
		}
		const auto origin = kind == Kind::MONTHS ? DEFAULT_ORIGIN_MICROS_2 : DEFAULT_ORIGIN_MICROS_1;
		return Dispatch(kind, width, origin, args.data[1], result, args.size());
	}

	static bool TryOffset(DataChunk &args, Vector &result) {
		interval_t width;
		Kind kind;
		auto &offset_arg = args.data[2];
		if (!TryGetConstantWidth(args.data[0], width, kind) || kind == Kind::MONTHS ||
		    offset_arg.GetVectorType() != VectorType::CONSTANT_VECTOR || ConstantVector::IsNull(offset_arg)) {
			return false;
		}
		const auto offset = *ConstantVector::GetData<interval_t>(offset_arg);
		if (offset.months != 0) {
			return false;
		}
		const auto shift = Interval::GetMicro(offset);
		return Execute(args.data[1], result, args.size(), [&](int64_t ts, int64_t &bucket) {
			int64_t shifted = 0;
			int64_t moved = 0;
			if (!TrySubtractOperator::Operation<int64_t, int64_t, int64_t>(ts, shift, shifted) ||
			    !TryBucket(kind, width, shifted, DEFAULT_ORIGIN_MICROS_1, bucket) ||
			    !TryAddOperator::Operation<int64_t, int64_t, int64_t>(bucket, shift, moved)) {
				return false;
			}
			bucket = moved;
			return true;
		});
	}

	static bool TryOrigin(DataChunk &args, Vector &result) {
		interval_t width;
		Kind kind;
		auto &origin_arg = args.data[2];
		if (!TryGetConstantWidth(args.data[0], width, kind) ||
		    origin_arg.GetVectorType() != VectorType::CONSTANT_VECTOR || ConstantVector::IsNull(origin_arg)) {
			return false;
		}
		const auto origin = *ConstantVector::GetData<timestamp_tz_t>(origin_arg);
		if (!origin.IsFinite()) {
			return false;
		}
		return Dispatch(kind, width, origin.value, args.data[1], result, args.size());
	}
};

} // namespace duckdb
