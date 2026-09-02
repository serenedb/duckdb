#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/compressed_materialization.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

namespace duckdb {

namespace {

idx_t RequiredBitsForValue(uint32_t n) {
	idx_t required_bits = 0;
	while (n > 0) {
		n >>= 1;
		required_bits++;
	}
	return required_bits;
}

bool TryAddPerfectHashBits(const hugeint_t &min, const hugeint_t &max, idx_t &total_bits) {
	uint64_t range = 0;
	if (max < min || !Hugeint::TryCast(max - min, range) || range >= NumericLimits<int32_t>::Maximum()) {
		return false;
	}
	total_bits += RequiredBitsForValue(UnsafeNumericCast<uint32_t>(range + 2));
	return true;
}

template <class T>
bool TryAddPerfectHashBits(const BaseStatistics &stats, idx_t &total_bits) {
	return TryAddPerfectHashBits(Hugeint::Convert(NumericStats::GetMin<T>(stats)),
	                             Hugeint::Convert(NumericStats::GetMax<T>(stats)), total_bits);
}

bool TryAddPerfectHashBits(const LogicalType &type, const BaseStatistics &stats, idx_t &total_bits) {
	switch (type.InternalType()) {
	case PhysicalType::INT8:
		return TryAddPerfectHashBits<int8_t>(stats, total_bits);
	case PhysicalType::INT16:
		return TryAddPerfectHashBits<int16_t>(stats, total_bits);
	case PhysicalType::INT32:
		return TryAddPerfectHashBits<int32_t>(stats, total_bits);
	case PhysicalType::INT64:
		return TryAddPerfectHashBits<int64_t>(stats, total_bits);
	case PhysicalType::UINT8:
		return TryAddPerfectHashBits<uint8_t>(stats, total_bits);
	case PhysicalType::UINT16:
		return TryAddPerfectHashBits<uint16_t>(stats, total_bits);
	case PhysicalType::UINT32:
		return TryAddPerfectHashBits<uint32_t>(stats, total_bits);
	case PhysicalType::UINT64:
		return TryAddPerfectHashBits<uint64_t>(stats, total_bits);
	default:
		return false;
	}
}

unique_ptr<BucketRewrite> GetBucketRewrite(ClientContext &context, const Expression &group) {
	switch (group.GetExpressionClass()) {
	case ExpressionClass::BOUND_FUNCTION: {
		auto &function = group.Cast<BoundFunctionExpression>();
		if (!function.Function().HasBucketRewriteCallback()) {
			return nullptr;
		}
		return function.Function().GetBucketRewriteCallback()(context, function);
	}
	case ExpressionClass::BOUND_CAST: {
		auto &cast = group.Cast<BoundCastExpression>();
		if (!cast.GetBoundCast().bucket_rewrite) {
			return nullptr;
		}
		return cast.GetBoundCast().bucket_rewrite(context, cast);
	}
	default:
		return nullptr;
	}
}

unique_ptr<Expression> &InputExpression(Expression &group, idx_t index) {
	if (group.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		return group.Cast<BoundCastExpression>().ChildMutable();
	}
	return group.Cast<BoundFunctionExpression>().GetChildrenMutable()[index];
}

struct BucketedGroup {
	idx_t group_idx;
	unique_ptr<BucketRewrite> rewrite;
	int64_t min;
	int64_t max;
	unique_ptr<BaseStatistics> stats;
};

} // namespace

void CompressedMaterialization::BucketDateTruncGroups(unique_ptr<LogicalOperator> &op) {
	auto &aggregate = op->Cast<LogicalAggregate>();
	if (aggregate.grouping_sets.size() > 1 || !aggregate.grouping_functions.empty()) {
		return;
	}
	auto &groups = aggregate.groups;
	auto &group_stats = aggregate.group_stats;
	if (groups.empty() || group_stats.size() != groups.size()) {
		return;
	}
	for (auto &expression : aggregate.expressions) {
		auto &aggr = expression->Cast<BoundAggregateExpression>();
		if (aggr.IsDistinct() || !aggr.Function().HasStateCombineCallback()) {
			return;
		}
	}

	vector<unique_ptr<BucketRewrite>> rewrites(groups.size());
	bool any_candidate = false;
	for (idx_t group_idx = 0; group_idx < groups.size(); group_idx++) {
		rewrites[group_idx] = GetBucketRewrite(context, *groups[group_idx]);
		any_candidate = any_candidate || rewrites[group_idx];
	}
	if (!any_candidate) {
		return;
	}

	auto input_stats = [&](const Expression &input,
	                       optional_ptr<const BaseStatistics> fallback) -> optional_ptr<const BaseStatistics> {
		if (input.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			auto it = statistics_map.find(input.Cast<BoundColumnRefExpression>().Binding());
			if (it != statistics_map.end() && it->second && NumericStats::HasMinMax(*it->second)) {
				return it->second.get();
			}
		}
		if (fallback && NumericStats::HasMinMax(*fallback)) {
			return fallback;
		}
		return nullptr;
	};
	idx_t total_bits = 0;
	vector<BucketedGroup> bucketed;
	for (idx_t group_idx = 0; group_idx < groups.size(); group_idx++) {
		auto &stats = group_stats[group_idx];
		auto &rewrite = rewrites[group_idx];
		if (rewrite) {
			auto &input = *InputExpression(*groups[group_idx], rewrite->InputIndex());
			const auto range_stats = input_stats(input, stats.get());
			BucketedGroup group {group_idx, std::move(rewrite), 0, 0, nullptr};
			if (!range_stats || !group.rewrite->TryBucketRange(*range_stats, group.min, group.max) ||
			    !TryAddPerfectHashBits(group.min, group.max, total_bits)) {
				return;
			}
			bucketed.push_back(std::move(group));
			continue;
		}
		if (!stats || !NumericStats::HasMinMax(*stats)) {
			return;
		}
		if (!TryAddPerfectHashBits(groups[group_idx]->GetReturnType(), *stats, total_bits)) {
			return;
		}
	}
	if (total_bits > Settings::Get<PerfectHtThresholdSetting>(context)) {
		return;
	}

	const auto old_bindings = aggregate.GetColumnBindings();
	for (auto &group : bucketed) {
		groups[group.group_idx] =
		    group.rewrite->Bucket(std::move(InputExpression(*groups[group.group_idx], group.rewrite->InputIndex())));
		auto bucket_stats = NumericStats::CreateEmpty(LogicalType::BIGINT);
		if (group_stats[group.group_idx]) {
			bucket_stats.CopyBase(*group_stats[group.group_idx]);
		}
		NumericStats::SetMin(bucket_stats, Value::BIGINT(group.min));
		NumericStats::SetMax(bucket_stats, Value::BIGINT(group.max));
		group.stats = std::move(group_stats[group.group_idx]);
		group_stats[group.group_idx] = bucket_stats.ToUnique();
		statistics_map[old_bindings[group.group_idx]] = bucket_stats.ToUnique();
		bucketed_groups.insert(old_bindings[group.group_idx]);
	}

	op->ResolveOperatorTypes();
	const auto &types = op->types;
	vector<unique_ptr<Expression>> projections;
	vector<optional_ptr<BaseStatistics>> statistics(old_bindings.size());
	vector<bool> is_bucketed(old_bindings.size(), false);
	for (idx_t col_idx = 0; col_idx < old_bindings.size(); col_idx++) {
		unique_ptr<Expression> expr = make_uniq<BoundColumnRefExpression>(types[col_idx], old_bindings[col_idx]);
		for (auto &group : bucketed) {
			if (group.group_idx == col_idx) {
				expr = group.rewrite->Unbucket(std::move(expr));
				statistics[col_idx] = group.stats.get();
				is_bucketed[col_idx] = true;
			}
		}
		projections.push_back(std::move(expr));
	}

	auto projection = make_uniq<LogicalProjection>(optimizer.binder.GenerateTableIndex(), std::move(projections));
	if (op->has_estimated_cardinality) {
		projection->SetEstimatedCardinality(op->estimated_cardinality);
	}
	const bool was_root = RefersToSameObject(*op, *root);
	projection->children.push_back(std::move(op));
	op = std::move(projection);
	op->ResolveOperatorTypes();
	const auto new_bindings = op->GetColumnBindings();
	for (idx_t col_idx = 0; col_idx < old_bindings.size(); col_idx++) {
		if (statistics[col_idx]) {
			statistics_map[new_bindings[col_idx]] = statistics[col_idx]->ToUnique();
			continue;
		}
		if (is_bucketed[col_idx]) {
			continue;
		}
		auto it = statistics_map.find(old_bindings[col_idx]);
		if (it != statistics_map.end() && it->second) {
			statistics_map[new_bindings[col_idx]] = it->second->ToUnique();
		}
	}
	if (was_root) {
		root = op;
		return;
	}

	ColumnBindingReplacer replacer;
	for (idx_t col_idx = 0; col_idx < old_bindings.size(); col_idx++) {
		replacer.replacement_bindings.emplace_back(old_bindings[col_idx], new_bindings[col_idx], op->types[col_idx]);
	}
	replacer.stop_operator = op.get();
	replacer.VisitOperator(*root);
}

} // namespace duckdb
