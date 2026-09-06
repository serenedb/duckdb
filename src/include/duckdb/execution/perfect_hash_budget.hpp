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

#include <absl/numeric/bits.h>

#include "duckdb/common/helper.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"

namespace duckdb {

struct PerfectHashBudget {
	static constexpr idx_t BYTES_PER_THREAD = idx_t(1) << 20;
	static constexpr idx_t MAX_BITS = 18;

	static idx_t RequiredBits(uint32_t n) {
		return absl::bit_width(n);
	}

	static idx_t StateBytes(const vector<unique_ptr<Expression>> &aggregates) {
		idx_t total = 0;
		for (auto &expr : aggregates) {
			auto &function = expr->Cast<BoundAggregateExpression>().Function();
			if (!function.HasStateSizeCallback()) {
				return NumericLimits<idx_t>::Maximum();
			}
			total += AlignValue(function.GetStateSizeCallback()(function));
		}
		return MaxValue<idx_t>(total, 1);
	}

	static idx_t MaxBits(ClientContext &context, const vector<unique_ptr<Expression>> &aggregates) {
		idx_t bits = Settings::Get<PerfectHtThresholdSetting>(context);
		const auto bytes = StateBytes(aggregates);
		if (bytes == NumericLimits<idx_t>::Maximum()) {
			return bits;
		}
		while (bits < MAX_BITS && (idx_t(1) << (bits + 1)) * bytes <= BYTES_PER_THREAD) {
			bits++;
		}
		return bits;
	}
};

} // namespace duckdb
