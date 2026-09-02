//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/perfect_hash_budget.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/helper.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"

namespace duckdb {

struct PerfectHashBudget {
	static constexpr idx_t BYTES_PER_THREAD = idx_t(1) << 20;
	static constexpr idx_t MAX_BITS = 18;

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
