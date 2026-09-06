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

#include <absl/algorithm/container.h>

#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/execution/perfect_hash_budget.hpp"
#include "duckdb/optimizer/bucket_composition.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/compressed_materialization.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

namespace duckdb {

namespace {

bool TryAddPerfectHashBits(const hugeint_t &min, const hugeint_t &max, idx_t &total_bits) {
	uint64_t range = 0;
	if (max < min || !Hugeint::TryCast(max - min, range) || range >= NumericLimits<int32_t>::Maximum()) {
		return false;
	}
	total_bits += PerfectHashBudget::RequiredBits(UnsafeNumericCast<uint32_t>(range + 2));
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

Expression &RewriteInput(Expression &group, const BucketRewrite &rewrite) {
	auto custom = rewrite.CustomInput();
	return custom ? *custom : *BucketRewriteInput(group, rewrite.InputIndex());
}

using statistics_map_t = column_binding_map_t<unique_ptr<BaseStatistics>>;

unique_ptr<BaseStatistics> CopyStatistics(const statistics_map_t &statistics_map, const ColumnBinding &binding) {
	auto it = statistics_map.find(binding);
	return it != statistics_map.end() && it->second ? it->second->ToUnique() : nullptr;
}

optional_ptr<const BaseStatistics> ColumnStatistics(const statistics_map_t &statistics_map, const Expression &expr) {
	if (expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return nullptr;
	}
	auto it = statistics_map.find(expr.Cast<BoundColumnRefExpression>().Binding());
	return it == statistics_map.end() ? nullptr : it->second.get();
}

bool UsableStatistics(optional_ptr<const BaseStatistics> stats) {
	return stats && stats->GetStatsType() == StatisticsType::NUMERIC_STATS && NumericStats::HasMinMax(*stats);
}

unique_ptr<BaseStatistics> ExpressionStatistics(ClientContext &context, const statistics_map_t &statistics_map,
                                                const Expression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF:
		return CopyStatistics(statistics_map, expr.Cast<BoundColumnRefExpression>().Binding());
	case ExpressionClass::BOUND_CONSTANT:
		return BaseStatistics::FromConstant(expr.Cast<BoundConstantExpression>().GetValue()).ToUnique();
	case ExpressionClass::BOUND_CAST: {
		auto &cast = expr.Cast<BoundCastExpression>();
		auto child = ExpressionStatistics(context, statistics_map, cast.Child());
		if (!child) {
			return nullptr;
		}
		auto result = StatisticsPropagator::TryPropagateCast(*child, cast.Child().GetReturnType(), cast.GetReturnType());
		if (result && cast.IsTryCast()) {
			result->Set(StatsInfo::CAN_HAVE_NULL_VALUES);
		}
		return result;
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &function = expr.Cast<BoundFunctionExpression>();
		if (!function.Function().HasStatisticsCallback()) {
			return nullptr;
		}
		vector<BaseStatistics> child_stats;
		for (auto &child : function.GetChildren()) {
			auto stats = ExpressionStatistics(context, statistics_map, *child);
			if (!stats) {
				return nullptr;
			}
			child_stats.push_back(std::move(*stats));
		}
		auto copy = expr.Copy();
		FunctionStatisticsInput input(copy->Cast<BoundFunctionExpression>(), function.BindInfo(), child_stats, &copy);
		return function.Function().GetStatisticsCallback()(context, input);
	}
	default:
		return nullptr;
	}
}

optional_ptr<const BaseStatistics> InputStatistics(ClientContext &context, const statistics_map_t &statistics_map,
                                                   vector<unique_ptr<BaseStatistics>> &derived, const Expression &input,
                                                   optional_ptr<const BaseStatistics> fallback) {
	auto stats = ColumnStatistics(statistics_map, input);
	if (!UsableStatistics(stats) && input.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		stats = ColumnStatistics(statistics_map, input.Cast<BoundCastExpression>().Child());
	}
	if (!UsableStatistics(stats) && input.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		if (auto computed = ExpressionStatistics(context, statistics_map, input)) {
			derived.push_back(std::move(computed));
			stats = derived.back().get();
		}
	}
	if (UsableStatistics(stats)) {
		return stats;
	}
	return UsableStatistics(fallback) ? fallback : nullptr;
}

struct BucketedGroup {
	idx_t group_idx;
	unique_ptr<BucketRewrite> rewrite;
	int64_t min;
	int64_t max;
	unique_ptr<BaseStatistics> stats;
	idx_t bits = 0;
	optional_idx provider;
	unique_ptr<Expression> shell;
	unique_ptr<Expression> input_template;
};

void CountReferences(const Expression &expr, column_binding_map_t<idx_t> &counts) {
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
	    expr, [&](const BoundColumnRefExpression &column) { counts[column.Binding()]++; });
}

using ExpressionSlot = optional_ptr<unique_ptr<Expression>>;

void CollectColumnSlots(unique_ptr<Expression> &expr, vector<ExpressionSlot> &slots) {
	ExpressionIterator::VisitExpressionClassMutable(expr, ExpressionClass::BOUND_COLUMN_REF,
	                                                [&](unique_ptr<Expression> &child) { slots.push_back(&child); });
}

unique_ptr<Expression> CanonicalShape(const Expression &expr) {
	auto copy = expr.Copy();
	vector<ExpressionSlot> slots;
	CollectColumnSlots(copy, slots);
	for (idx_t i = 0; i < slots.size(); i++) {
		*slots[i] = make_uniq<BoundReferenceExpression>((*slots[i])->GetReturnType(), i);
	}
	return copy;
}

bool SameShape(const Expression &left, const Expression &right) {
	return left.GetReturnType() == right.GetReturnType() && CanonicalShape(left)->Equals(*CanonicalShape(right));
}

struct GroupAlias {
	vector<ExpressionSlot> definitions;
	ExpressionSlot wrapper_slot;
	vector<ExpressionSlot> links;

	bool Valid() const {
		return !definitions.empty();
	}
	bool SpansUnion() const {
		return definitions.size() > 1;
	}
	bool Admits(const BucketRewrite &rewrite, const Expression &input, const LogicalType &group_type) const {
		return !SpansUnion() || (!rewrite.CustomInput() && input.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
		                         input.GetReturnType() == group_type);
	}
	unique_ptr<Expression> &Definition() {
		return *definitions[0];
	}
};

vector<vector<unique_ptr<BucketRewrite>>> CollectCandidates(ClientContext &context,
                                                           const vector<reference<Expression>> &groups,
                                                           const vector<unique_ptr<BaseStatistics>> &group_stats,
                                                           idx_t max_bits) {
	auto coordinate_rewrites = CoordinateBucketRewrites(context, groups);
	vector<vector<unique_ptr<BucketRewrite>>> candidates(groups.size());
	for (idx_t group_idx = 0; group_idx < groups.size(); group_idx++) {
		auto &group = groups[group_idx].get();
		auto &stats = group_stats[group_idx];
		idx_t dense_bits = 0;
		const bool dense_already = group.GetReturnType().IsIntegral() && UsableStatistics(stats.get()) &&
		                           TryAddPerfectHashBits(group.GetReturnType(), *stats, dense_bits) &&
		                           dense_bits <= max_bits;
		if (coordinate_rewrites[group_idx]) {
			candidates[group_idx].push_back(std::move(coordinate_rewrites[group_idx]));
		}
		if (auto hooked = GetHookedBucketRewrite(context, group)) {
			const bool cheap_date = RewriteInput(group, *hooked).GetReturnType().id() == LogicalTypeId::DATE &&
			                        group.GetReturnType().IsTemporal();
			if (!cheap_date) {
				candidates[group_idx].push_back(std::move(hooked));
			}
		}
		if (!dense_already) {
			for (auto &composite : CompositeBucketRewrites(context, group)) {
				candidates[group_idx].push_back(std::move(composite));
			}
		}
	}
	return candidates;
}

idx_t EliminateNested(vector<BucketedGroup> &bucketed, const vector<reference<Expression>> &groups) {
	idx_t saved_bits = 0;
	for (idx_t coarse_idx = 0; coarse_idx < bucketed.size(); coarse_idx++) {
		auto &coarse_group = bucketed[coarse_idx];
		auto &coarse = *coarse_group.rewrite;
		const auto coarse_grid = coarse.Grid();
		if (coarse_grid.family == BucketGrid::Family::NONE) {
			continue;
		}
		auto &coarse_input = RewriteInput(groups[coarse_group.group_idx], coarse);
		for (idx_t fine_idx = 0; fine_idx < bucketed.size(); fine_idx++) {
			auto &fine_group = bucketed[fine_idx];
			auto &fine = *fine_group.rewrite;
			if (fine_idx == coarse_idx || fine_group.provider.IsValid() || !coarse.Contains(fine.Grid()) ||
			    (fine.Contains(coarse_grid) && fine_idx > coarse_idx) ||
			    !coarse_input.Equals(RewriteInput(groups[fine_group.group_idx], fine))) {
				continue;
			}
			coarse_group.provider = fine_idx;
			coarse_group.shell = groups[coarse_group.group_idx].get().Copy();
			coarse_group.input_template = coarse_input.Copy();
			saved_bits += coarse_group.bits;
			break;
		}
	}
	for (auto &group : bucketed) {
		while (group.provider.IsValid() && bucketed[group.provider.GetIndex()].provider.IsValid()) {
			group.provider = bucketed[group.provider.GetIndex()].provider;
		}
	}
	return saved_bits;
}

class AliasResolver {
public:
	static constexpr idx_t MAX_DEPTH = 4;

	AliasResolver(const LogicalAggregate &aggregate, LogicalOperator &child) {
		references.emplace_back();
		for (auto &group : aggregate.groups) {
			CountReferences(*group, references.back());
		}
		for (auto &expression : aggregate.expressions) {
			CountReferences(*expression, references.back());
		}
		reference<LogicalOperator> node(child);
		while (node.get().type == LogicalOperatorType::LOGICAL_PROJECTION && projections.size() < MAX_DEPTH) {
			auto &projection = node.get().Cast<LogicalProjection>();
			projections.emplace_back(projection);
			references.emplace_back();
			for (auto &expression : projection.expressions) {
				CountReferences(*expression, references.back());
			}
			if (projection.children.empty()) {
				break;
			}
			node = *projection.children[0];
		}
		if (node.get().type == LogicalOperatorType::LOGICAL_UNION && node.get().Cast<LogicalSetOperation>().setop_all) {
			union_op = &node.get().Cast<LogicalSetOperation>();
		}
	}

	GroupAlias Resolve(unique_ptr<Expression> &group) const {
		GroupAlias alias;
		ExpressionSlot current = &group;
		if (group->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			vector<ExpressionSlot> slots;
			CollectColumnSlots(group, slots);
			if (slots.size() != 1) {
				return GroupAlias();
			}
			current = slots[0];
			alias.wrapper_slot = slots[0];
		}
		idx_t level = 0;
		for (; level < projections.size(); level++) {
			if ((*current)->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				break;
			}
			auto &projection = projections[level].get();
			const auto &binding = (*current)->Cast<BoundColumnRefExpression>().Binding();
			if (binding.table_index != projection.table_index || !SingleReference(level, binding) ||
			    binding.column_index >= projection.expressions.size()) {
				break;
			}
			if (level > 0) {
				alias.links.push_back(current);
			}
			current = &projection.expressions[binding.column_index];
			alias.definitions.assign(1, current);
		}
		if (union_op && level == projections.size() &&
		    (*current)->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			const auto &binding = (*current)->Cast<BoundColumnRefExpression>().Binding();
			if (binding.table_index == union_op->table_index && SingleReference(level, binding)) {
				vector<ExpressionSlot> branch_links;
				auto branches = UnionDefinitions(binding.column_index, branch_links);
				if (!branches.empty()) {
					if (level > 0) {
						alias.links.push_back(current);
					}
					alias.links.insert(alias.links.end(), branch_links.begin(), branch_links.end());
					alias.definitions = std::move(branches);
				}
			}
		}
		if (!alias.Valid() || alias.Definition()->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			return GroupAlias();
		}
		return alias;
	}

private:
	bool SingleReference(idx_t level, const ColumnBinding &binding) const {
		auto it = references[level].find(binding);
		return it != references[level].end() && it->second == 1;
	}

	vector<ExpressionSlot> UnionDefinitions(idx_t column_index, vector<ExpressionSlot> &branch_links) const {
		vector<ExpressionSlot> branches;
		for (auto &branch : union_op->children) {
			reference<LogicalOperator> node(*branch);
			idx_t column = column_index;
			ExpressionSlot slot;
			for (idx_t depth = 0; depth < MAX_DEPTH; depth++) {
				if (node.get().type != LogicalOperatorType::LOGICAL_PROJECTION ||
				    column >= node.get().Cast<LogicalProjection>().expressions.size()) {
					slot = nullptr;
					break;
				}
				auto &projection = node.get().Cast<LogicalProjection>();
				slot = &projection.expressions[column];
				if ((*slot)->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF || projection.children.empty() ||
				    projection.children[0]->type != LogicalOperatorType::LOGICAL_PROJECTION) {
					break;
				}
				const auto &inner = (*slot)->Cast<BoundColumnRefExpression>().Binding();
				if (inner.table_index != projection.children[0]->Cast<LogicalProjection>().table_index) {
					break;
				}
				branch_links.push_back(slot);
				column = inner.column_index;
				node = *projection.children[0];
			}
			if (!slot || (*slot)->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF ||
			    (!branches.empty() && !SameShape(**branches[0], **slot))) {
				return {};
			}
			branches.push_back(slot);
		}
		return branches;
	}

	vector<reference<LogicalProjection>> projections;
	vector<column_binding_map_t<idx_t>> references;
	optional_ptr<LogicalSetOperation> union_op;
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

	const AliasResolver resolver(aggregate, *op->children[0]);
	vector<GroupAlias> aliases;
	vector<unique_ptr<Expression>> synthesized(groups.size());
	vector<reference<Expression>> analysed_groups;
	for (idx_t group_idx = 0; group_idx < groups.size(); group_idx++) {
		aliases.push_back(resolver.Resolve(groups[group_idx]));
		auto &alias = aliases.back();
		if (!alias.Valid()) {
			analysed_groups.emplace_back(*groups[group_idx]);
		} else if (!alias.wrapper_slot) {
			analysed_groups.emplace_back(*alias.Definition());
		} else {
			auto copy = groups[group_idx]->Copy();
			vector<ExpressionSlot> slots;
			CollectColumnSlots(copy, slots);
			*slots[0] = alias.Definition()->Copy();
			synthesized[group_idx] = std::move(copy);
			analysed_groups.emplace_back(*synthesized[group_idx]);
		}
	}
	auto analysed = [&](idx_t group_idx) -> Expression & {
		return analysed_groups[group_idx].get();
	};

	const auto max_bits = PerfectHashBudget::MaxBits(context, aggregate.expressions);
	auto candidates = CollectCandidates(context, analysed_groups, group_stats, max_bits);
	if (absl::c_all_of(candidates, [](const vector<unique_ptr<BucketRewrite>> &options) { return options.empty(); })) {
		return;
	}

	vector<unique_ptr<BaseStatistics>> derived_stats;
	idx_t total_bits = 0;
	vector<BucketedGroup> bucketed;
	for (idx_t group_idx = 0; group_idx < groups.size(); group_idx++) {
		auto &stats = group_stats[group_idx];
		auto &options = candidates[group_idx];
		if (!options.empty()) {
			BucketedGroup group {group_idx, nullptr, 0, 0, nullptr};
			const auto &alias = aliases[group_idx];
			const auto &group_type = analysed(group_idx).GetReturnType();
			for (auto &candidate : options) {
				idx_t bits = total_bits;
				auto &input = RewriteInput(analysed(group_idx), *candidate);
				if (!alias.Admits(*candidate, input, group_type)) {
					continue;
				}
				bool ranged = candidate->TryConstantRange(group.min, group.max);
				if (!ranged) {
					const auto range_stats =
					    alias.SpansUnion()
					        ? (UsableStatistics(stats.get()) ? stats.get() : nullptr)
					        : InputStatistics(context, statistics_map, derived_stats, input,
					                          candidate->CustomInput() ? nullptr : stats.get());
					ranged = range_stats && candidate->TryBucketRange(*range_stats, group.min, group.max);
				}
				if (ranged && TryAddPerfectHashBits(group.min, group.max, bits)) {
					group.rewrite = std::move(candidate);
					group.bits = bits - total_bits;
					total_bits = bits;
					break;
				}
			}
			if (!group.rewrite) {
				return;
			}
			bucketed.push_back(std::move(group));
			continue;
		}
		if (!UsableStatistics(stats.get())) {
			return;
		}
		if (!TryAddPerfectHashBits(groups[group_idx]->GetReturnType(), *stats, total_bits)) {
			return;
		}
	}

	total_bits -= EliminateNested(bucketed, analysed_groups);

	if (total_bits > max_bits) {
		return;
	}

	const auto old_bindings = aggregate.GetColumnBindings();
	vector<bool> removed(old_bindings.size(), false);
	bool projection_changed = false;
	for (auto &group : bucketed) {
		group.stats = std::move(group_stats[group.group_idx]);
		auto &alias = aliases[group.group_idx];
		if (group.provider.IsValid()) {
			removed[group.group_idx] = true;
			for (auto &definition : alias.definitions) {
				*definition = make_uniq<BoundConstantExpression>(Value((*definition)->GetReturnType()));
				projection_changed = true;
			}
			continue;
		}
		auto bucket = group.rewrite->Bucket(RewriteInput(analysed(group.group_idx), *group.rewrite).Copy());
		auto bucket_stats = NumericStats::CreateEmpty(LogicalType::BIGINT);
		if (group.stats) {
			bucket_stats.CopyBase(*group.stats);
		}
		NumericStats::SetMin(bucket_stats, Value::BIGINT(group.min));
		NumericStats::SetMax(bucket_stats, Value::BIGINT(group.max));
		if (alias.Valid()) {
			for (idx_t branch = 1; branch < alias.definitions.size(); branch++) {
				auto &input = *BucketRewriteInput(**alias.definitions[branch], group.rewrite->InputIndex());
				*alias.definitions[branch] = group.rewrite->Bucket(input.Copy());
			}
			alias.Definition() = std::move(bucket);
			for (auto &link : alias.links) {
				const auto binding = (*link)->Cast<BoundColumnRefExpression>().Binding();
				*link = make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT, binding);
				statistics_map[binding] = bucket_stats.ToUnique();
			}
			auto &group_slot = alias.wrapper_slot ? *alias.wrapper_slot : groups[group.group_idx];
			const auto binding = group_slot->Cast<BoundColumnRefExpression>().Binding();
			groups[group.group_idx] = make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT, binding);
			statistics_map[binding] = bucket_stats.ToUnique();
			projection_changed = true;
		} else {
			groups[group.group_idx] = std::move(bucket);
		}
		group_stats[group.group_idx] = bucket_stats.ToUnique();
		statistics_map[old_bindings[group.group_idx]] = bucket_stats.ToUnique();
		bucketed_groups.insert(old_bindings[group.group_idx]);
	}
	if (projection_changed) {
		op->children[0]->ResolveOperatorTypes();
	}
	for (idx_t group_idx = groups.size(); group_idx > 0; group_idx--) {
		if (removed[group_idx - 1]) {
			groups.erase(groups.begin() + NumericCast<int64_t>(group_idx - 1));
			group_stats.erase(group_stats.begin() + NumericCast<int64_t>(group_idx - 1));
		}
	}
	if (!aggregate.grouping_sets.empty()) {
		GroupingSet all;
		for (idx_t group_idx = 0; group_idx < groups.size(); group_idx++) {
			all.insert(ProjectionIndex(group_idx));
		}
		aggregate.grouping_sets[0] = std::move(all);
	}

	op->ResolveOperatorTypes();
	const auto &types = op->types;
	const auto kept_bindings = op->GetColumnBindings();
	vector<idx_t> kept_position(old_bindings.size(), DConstants::INVALID_INDEX);
	for (idx_t col_idx = 0, kept = 0; col_idx < old_bindings.size(); col_idx++) {
		if (!removed[col_idx]) {
			kept_position[col_idx] = kept++;
		}
	}
	for (idx_t col_idx = 0; col_idx < old_bindings.size(); col_idx++) {
		if (removed[col_idx] || kept_bindings[kept_position[col_idx]] == old_bindings[col_idx]) {
			continue;
		}
		const auto &moved = kept_bindings[kept_position[col_idx]];
		if (auto stats = CopyStatistics(statistics_map, old_bindings[col_idx])) {
			statistics_map[moved] = std::move(stats);
		} else {
			statistics_map.erase(moved);
		}
		if (bucketed_groups.erase(old_bindings[col_idx])) {
			bucketed_groups.insert(moved);
		}
	}
	auto kept_reference = [&](idx_t col_idx) -> unique_ptr<Expression> {
		const auto position = kept_position[col_idx];
		return make_uniq<BoundColumnRefExpression>(types[position], kept_bindings[position]);
	};
	vector<optional_ptr<BucketedGroup>> bucket_of(old_bindings.size());
	for (auto &group : bucketed) {
		bucket_of[group.group_idx] = &group;
	}
	vector<unique_ptr<Expression>> projections;
	for (idx_t col_idx = 0; col_idx < old_bindings.size(); col_idx++) {
		auto group = bucket_of[col_idx];
		if (!group) {
			projections.push_back(kept_reference(col_idx));
		} else if (!group->provider.IsValid()) {
			projections.push_back(group->rewrite->Unbucket(kept_reference(col_idx)));
		} else {
			auto &provider = bucketed[group->provider.GetIndex()];
			projections.push_back(
			    RebuildShell(context, *group->shell, *group->input_template,
			                 provider.rewrite->UnbucketCore(kept_reference(provider.group_idx))));
		}
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
		if (auto group = bucket_of[col_idx]) {
			if (group->stats) {
				statistics_map[new_bindings[col_idx]] = std::move(group->stats);
			}
			continue;
		}
		if (auto stats = CopyStatistics(statistics_map, old_bindings[col_idx])) {
			statistics_map[new_bindings[col_idx]] = std::move(stats);
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
