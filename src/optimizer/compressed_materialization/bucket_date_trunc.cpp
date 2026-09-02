#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/operator/date_trunc_operators.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/function/scalar/date_functions.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/compressed_materialization.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

namespace duckdb {

namespace {

bool TryGetBucketWidth(const Expression &expr, int64_t &width, int64_t &anchor) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &function = expr.Cast<BoundFunctionExpression>();
	const auto &name = function.Function().GetName();
	if (name != "date_trunc" && name != "datetrunc") {
		return false;
	}
	auto &children = function.GetChildren();
	if (children.size() != 2 || children[0]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
	    children[1]->GetReturnType().id() != LogicalTypeId::TIMESTAMP) {
		return false;
	}
	const auto &unit = children[0]->Cast<BoundConstantExpression>().GetValue();
	DatePartSpecifier part;
	if (unit.IsNull() || !TryGetDatePartSpecifier(StringValue::Get(unit), part)) {
		return false;
	}
	anchor = 0;
	switch (part) {
	case DatePartSpecifier::SECOND:
	case DatePartSpecifier::EPOCH:
		width = Interval::MICROS_PER_SEC;
		return true;
	case DatePartSpecifier::MINUTE:
		width = Interval::MICROS_PER_MINUTE;
		return true;
	case DatePartSpecifier::HOUR:
		width = Interval::MICROS_PER_HOUR;
		return true;
	case DatePartSpecifier::DAY:
	case DatePartSpecifier::DOW:
	case DatePartSpecifier::ISODOW:
	case DatePartSpecifier::DOY:
	case DatePartSpecifier::JULIAN_DAY:
		width = Interval::MICROS_PER_DAY;
		return true;
	case DatePartSpecifier::WEEK:
	case DatePartSpecifier::YEARWEEK:
		width = Interval::MICROS_PER_WEEK;
		anchor = DateTrunc::EPOCH_MONDAY * Interval::MICROS_PER_DAY;
		return true;
	default:
		return false;
	}
}

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

unique_ptr<Expression> MakeCall(ScalarFunction function, unique_ptr<Expression> input, int64_t width, int64_t anchor) {
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(std::move(input));
	arguments.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(width)));
	arguments.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(anchor)));
	BoundScalarFunction bound_function(std::move(function));
	return make_uniq<BoundFunctionExpression>(std::move(bound_function), std::move(arguments), nullptr);
}

struct BucketedGroup {
	idx_t group_idx;
	int64_t width;
	int64_t anchor;
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

	const auto limit = NumericLimits<int64_t>::Maximum() - 2 * Interval::MICROS_PER_WEEK;
	idx_t total_bits = 0;
	vector<BucketedGroup> bucketed;
	for (idx_t group_idx = 0; group_idx < groups.size(); group_idx++) {
		auto &stats = group_stats[group_idx];
		if (!stats || !NumericStats::HasMinMax(*stats)) {
			return;
		}
		int64_t width = 0;
		int64_t anchor = 0;
		if (TryGetBucketWidth(*groups[group_idx], width, anchor)) {
			const auto min = NumericStats::GetMin<timestamp_t>(*stats);
			const auto max = NumericStats::GetMax<timestamp_t>(*stats);
			if (min > max || min.value < -limit || max.value > limit) {
				return;
			}
			BucketedGroup group {group_idx,
			                     width,
			                     anchor,
			                     DateTrunc::FloorDiv(min.value - anchor, width),
			                     DateTrunc::FloorDiv(max.value - anchor, width),
			                     nullptr};
			if (!TryAddPerfectHashBits(group.min, group.max, total_bits)) {
				return;
			}
			bucketed.push_back(std::move(group));
			continue;
		}
		if (!TypeIsIntegral(groups[group_idx]->GetReturnType().InternalType()) ||
		    !TryAddPerfectHashBits(NumericStats::Min(*stats).GetValue<hugeint_t>(),
		                           NumericStats::Max(*stats).GetValue<hugeint_t>(), total_bits)) {
			return;
		}
	}
	if (bucketed.empty() || total_bits > Settings::Get<PerfectHtThresholdSetting>(context)) {
		return;
	}

	const auto old_bindings = aggregate.GetColumnBindings();
	for (auto &group : bucketed) {
		auto &function = groups[group.group_idx]->Cast<BoundFunctionExpression>();
		groups[group.group_idx] = MakeCall(InternalDateTruncBucketFun::GetFunction(),
		                                   std::move(function.GetChildrenMutable()[1]), group.width, group.anchor);
		auto bucket_stats = NumericStats::CreateEmpty(LogicalType::BIGINT);
		bucket_stats.CopyBase(*group_stats[group.group_idx]);
		NumericStats::SetMin(bucket_stats, Value::BIGINT(group.min));
		NumericStats::SetMax(bucket_stats, Value::BIGINT(group.max));
		group.stats = std::move(group_stats[group.group_idx]);
		group_stats[group.group_idx] = bucket_stats.ToUnique();
		statistics_map[old_bindings[group.group_idx]] = bucket_stats.ToUnique();
	}

	op->ResolveOperatorTypes();
	const auto &types = op->types;
	vector<unique_ptr<Expression>> projections;
	vector<optional_ptr<BaseStatistics>> statistics(old_bindings.size());
	for (idx_t col_idx = 0; col_idx < old_bindings.size(); col_idx++) {
		unique_ptr<Expression> expr = make_uniq<BoundColumnRefExpression>(types[col_idx], old_bindings[col_idx]);
		for (auto &group : bucketed) {
			if (group.group_idx == col_idx) {
				expr =
				    MakeCall(InternalDateTruncUnbucketFun::GetFunction(), std::move(expr), group.width, group.anchor);
				statistics[col_idx] = group.stats.get();
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
