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

#include "duckdb/optimizer/compressed_materialization.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

bool CompressedMaterialization::BucketDistinct(unique_ptr<LogicalOperator> &op) {
	auto &distinct = op->Cast<LogicalDistinct>();
	if (distinct.distinct_type != DistinctType::DISTINCT || distinct.order_by || distinct.children.size() != 1 ||
	    distinct.children[0]->type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return false;
	}
	auto &projection = distinct.children[0]->Cast<LogicalProjection>();
	auto &targets = distinct.distinct_targets;
	if (targets.size() != projection.expressions.size() || projection.children.size() != 1) {
		return false;
	}
	for (idx_t col_idx = 0; col_idx < targets.size(); col_idx++) {
		if (targets[col_idx]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return false;
		}
		const auto &binding = targets[col_idx]->Cast<BoundColumnRefExpression>().Binding();
		if (binding.table_index != projection.table_index || binding.column_index != col_idx) {
			return false;
		}
	}

	auto aggregate = make_uniq<LogicalAggregate>(projection.table_index, optimizer.binder.GenerateTableIndex(),
	                                             vector<unique_ptr<Expression>>());
	aggregate->groups = std::move(projection.expressions);
	GroupingSet all;
	for (idx_t col_idx = 0; col_idx < aggregate->groups.size(); col_idx++) {
		all.insert(ProjectionIndex(col_idx));
		auto it = statistics_map.find(ColumnBinding(projection.table_index, ProjectionIndex(col_idx)));
		aggregate->group_stats.push_back(it != statistics_map.end() && it->second ? it->second->ToUnique() : nullptr);
	}
	aggregate->grouping_sets.push_back(std::move(all));
	aggregate->children.push_back(std::move(projection.children[0]));
	if (distinct.has_estimated_cardinality) {
		aggregate->SetEstimatedCardinality(distinct.estimated_cardinality);
	}

	const bool was_root = RefersToSameObject(*op, *root);
	auto original = std::move(op);
	op = std::move(aggregate);
	op->ResolveOperatorTypes();
	if (was_root) {
		root = op;
	}
	BucketDateTruncGroups(op);
	if (op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		return true;
	}

	auto &restored = op->Cast<LogicalAggregate>();
	projection.expressions = std::move(restored.groups);
	projection.children[0] = std::move(restored.children[0]);
	op = std::move(original);
	if (was_root) {
		root = op;
	}
	return false;
}

} // namespace duckdb
