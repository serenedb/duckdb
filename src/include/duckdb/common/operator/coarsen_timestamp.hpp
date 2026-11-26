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

#include "duckdb/common/types/timestamp_base.hpp"

namespace duckdb {

template <class SRC, class DST>
inline DST CoarsenTimestamp(SRC input) {
	static_assert(SRC::PRECISION > DST::PRECISION, "CoarsenTimestamp only coarsens");
	if (!input.IsFinite()) {
		return DST(input.value);
	}
	//	Round away from the epoch.
	//	Scale first so we don't overflow.
	constexpr int64_t scaling = SRC::PRECISION / DST::PRECISION / 2;
	auto value = input.value / scaling;
	value += value < 0 ? -1 : 1;
	return DST(value / 2);
}

struct CoarsenTimestampOperator {
	template <class SRC, class DST>
	static inline DST Operation(SRC input) {
		return CoarsenTimestamp<SRC, DST>(input);
	}
};

} // namespace duckdb
