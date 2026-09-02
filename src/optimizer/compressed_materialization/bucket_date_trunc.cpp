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

struct BucketSpec {
	bool calendar = false;
	int64_t width = 0;
	int64_t anchor = 0;

	int64_t Bucket(timestamp_t ts) const {
		return DateTrunc::FloorDiv((calendar ? DateTrunc::MonthIndex(ts) : ts.value) - anchor, width);
	}
	ScalarFunction BucketFunction() const {
		return calendar ? InternalDateTruncMonthBucketFun::GetFunction() : InternalDateTruncBucketFun::GetFunction();
	}
	ScalarFunction UnbucketFunction() const {
		return calendar ? InternalDateTruncMonthUnbucketFun::GetFunction()
		                : InternalDateTruncUnbucketFun::GetFunction();
	}
};

bool TryGetBucketSpec(const Expression &expr, BucketSpec &spec) {
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
	spec = BucketSpec();
	switch (part) {
	case DatePartSpecifier::SECOND:
	case DatePartSpecifier::EPOCH:
		spec.width = Interval::MICROS_PER_SEC;
		return true;
	case DatePartSpecifier::MINUTE:
		spec.width = Interval::MICROS_PER_MINUTE;
		return true;
	case DatePartSpecifier::HOUR:
		spec.width = Interval::MICROS_PER_HOUR;
		return true;
	case DatePartSpecifier::DAY:
	case DatePartSpecifier::DOW:
	case DatePartSpecifier::ISODOW:
	case DatePartSpecifier::DOY:
	case DatePartSpecifier::JULIAN_DAY:
		spec.width = Interval::MICROS_PER_DAY;
		return true;
	case DatePartSpecifier::WEEK:
	case DatePartSpecifier::YEARWEEK:
		spec.width = Interval::MICROS_PER_WEEK;
		spec.anchor = DateTrunc::EPOCH_MONDAY * Interval::MICROS_PER_DAY;
		return true;
	case DatePartSpecifier::MONTH:
		spec.calendar = true;
		spec.width = 1;
		return true;
	case DatePartSpecifier::QUARTER:
		spec.calendar = true;
		spec.width = Interval::MONTHS_PER_QUARTER;
		return true;
	case DatePartSpecifier::YEAR:
		spec.calendar = true;
		spec.width = Interval::MONTHS_PER_YEAR;
		return true;
	case DatePartSpecifier::DECADE:
		spec.calendar = true;
		spec.width = Interval::MONTHS_PER_DECADE;
		return true;
	case DatePartSpecifier::CENTURY:
		spec.calendar = true;
		spec.width = Interval::MONTHS_PER_CENTURY;
		return true;
	case DatePartSpecifier::MILLENNIUM:
		spec.calendar = true;
		spec.width = Interval::MONTHS_PER_MILLENIUM;
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
	BucketSpec spec;
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

	vector<idx_t> candidates;
	for (idx_t group_idx = 0; group_idx < groups.size(); group_idx++) {
		BucketSpec spec;
		if (TryGetBucketSpec(*groups[group_idx], spec)) {
			candidates.push_back(group_idx);
		}
	}
	if (candidates.empty()) {
		return;
	}

	const auto limit = NumericLimits<int64_t>::Maximum() - 2 * Interval::MICROS_PER_WEEK;
	const auto first_ad = DateTrunc::YearStart(1) * Interval::MICROS_PER_DAY;
	auto input_stats = [&](const Expression &group, const BaseStatistics &fallback) -> const BaseStatistics & {
		auto &input = *group.Cast<BoundFunctionExpression>().GetChildren()[1];
		if (input.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			auto it = statistics_map.find(input.Cast<BoundColumnRefExpression>().Binding());
			if (it != statistics_map.end() && it->second && NumericStats::HasMinMax(*it->second)) {
				return *it->second;
			}
		}
		return fallback;
	};
	idx_t total_bits = 0;
	vector<BucketedGroup> bucketed;
	for (idx_t group_idx = 0; group_idx < groups.size(); group_idx++) {
		auto &stats = group_stats[group_idx];
		if (!stats || !NumericStats::HasMinMax(*stats)) {
			return;
		}
		BucketSpec spec;
		if (TryGetBucketSpec(*groups[group_idx], spec)) {
			auto &range_stats = input_stats(*groups[group_idx], *stats);
			const auto min = NumericStats::GetMin<timestamp_t>(range_stats);
			const auto max = NumericStats::GetMax<timestamp_t>(range_stats);
			if (min > max || min.value < (spec.calendar ? first_ad : -limit) || max.value > limit) {
				return;
			}
			BucketedGroup group {group_idx, spec, spec.Bucket(min), spec.Bucket(max), nullptr};
			if (!TryAddPerfectHashBits(group.min, group.max, total_bits)) {
				return;
			}
			bucketed.push_back(std::move(group));
			continue;
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
		auto &function = groups[group.group_idx]->Cast<BoundFunctionExpression>();
		groups[group.group_idx] = MakeCall(group.spec.BucketFunction(), std::move(function.GetChildrenMutable()[1]),
		                                   group.spec.width, group.spec.anchor);
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
				expr = MakeCall(group.spec.UnbucketFunction(), std::move(expr), group.spec.width, group.spec.anchor);
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
