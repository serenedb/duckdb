#include "duckdb/optimizer/cte_filter_pusher.hpp"
#include "duckdb/optimizer/cte_join_filter_pusher.hpp"

#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/expression_barrier.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"

namespace duckdb {

CTEFilterPusher::MaterializedCTEInfo::MaterializedCTEInfo(LogicalOperator &materialized_cte_p)
    : materialized_cte(materialized_cte_p),
      has_filter_dependency(materialized_cte_p.Cast<LogicalMaterializedCTE>().filter_dependency != nullptr) {
}

vector<reference<LogicalOperator>> CTEFilterPusher::MaterializedCTEInfo::Filters() {
	vector<reference<LogicalOperator>> result;
	for (auto &entry : references) {
		if (entry.second.filter) {
			result.push_back(*entry.second.filter);
		}
	}
	return result;
}

vector<reference<LogicalCTERef>> CTEFilterPusher::MaterializedCTEInfo::References() const {
	vector<reference<LogicalCTERef>> result;
	result.reserve(references.size());
	for (auto &entry : references) {
		result.push_back(entry.second.ref);
	}
	return result;
}

bool CTEFilterPusher::MaterializedCTEInfo::AllReferencesAreFiltered() const {
	for (auto &entry : references) {
		if (!entry.second.filter) {
			return false;
		}
	}
	return true;
}

CTEFilterPusher::CTEFilterPusher(Optimizer &optimizer_p) : optimizer(optimizer_p) {
}

static void CollectReferencedCTEs(const LogicalOperator &op, unordered_set<TableIndex> &referenced_ctes) {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		referenced_ctes.insert(op.Cast<LogicalCTERef>().cte_index);
	}
	for (auto &child : op.children) {
		CollectReferencedCTEs(*child, referenced_ctes);
	}
}

unique_ptr<LogicalOperator> CTEFilterPusher::Optimize(unique_ptr<LogicalOperator> op) {
	FindCandidates(*op);
	if (cte_info_map.empty()) {
		return op;
	}
	// Scalar producer pushdown must preserve markers referenced by CTE consumers.
	FilterPushdown pushdown(optimizer);
	unordered_set<TableIndex> referenced_bindings;
	pushdown.CheckMarkToSemi(*op, referenced_bindings);

	vector<reference<MaterializedCTEInfo>> candidates;
	for (auto &entry : cte_info_map) {
		auto &info = *entry.second;
		if (info.Filters().empty() || (!info.AllReferencesAreFiltered() && !info.has_filter_dependency)) {
			continue;
		}
		candidates.push_back(info);
	}

	// Iterate once over all materialized CTEs
	for (idx_t candidate_idx = candidates.size(); candidate_idx > 0; candidate_idx--) {
		auto &info = candidates[candidate_idx - 1].get();
		if (!CanPushFilter(info)) {
			continue;
		}
		unordered_set<TableIndex> referenced_ctes;
		CollectReferencedCTEs(*info.materialized_cte.children[0], referenced_ctes);
		PushFilterIntoCTE(info);
		RefreshDefinition(info, referenced_ctes);
#ifdef D_ASSERT_IS_ENABLED
		if (DUCKDB_DEBUG_VERIFY_ENABLED()) {
			CTEFilterPusher fresh(optimizer);
			fresh.FindCandidates(*op);
			for (auto &fresh_entry : fresh.cte_info_map) {
				auto current = cte_info_map.find(fresh_entry.first);
				D_ASSERT(current != cte_info_map.end());
				auto &fresh_references = fresh_entry.second->references;
				auto &current_references = current->second->references;
				D_ASSERT(fresh_references.size() == current_references.size());
				for (auto fresh_ref = fresh_references.begin(), current_ref = current_references.begin();
				     fresh_ref != fresh_references.end(); fresh_ref++, current_ref++) {
					D_ASSERT(fresh_ref->second.filter.get() == current_ref->second.filter.get());
					D_ASSERT(&fresh_ref->second.ref.get() == &current_ref->second.ref.get());
				}
			}
		}
#endif
	}
	CTEJoinFilterPusher join_pusher(optimizer);
	join_pusher.Optimize(*op);
	return op;
}

void CTEFilterPusher::RefreshDefinition(MaterializedCTEInfo &info, const unordered_set<TableIndex> &referenced_ctes) {
	for (auto &cte_index : referenced_ctes) {
		auto entry = cte_info_map.find(to_string(cte_index.index));
		if (entry == cte_info_map.end()) {
			continue;
		}
		auto &references = entry->second->references;
		references.erase(references.lower_bound(info.definition_begin), references.lower_bound(info.definition_end));
	}
	position = info.definition_begin;
	refreshing = true;
	FindCandidates(*info.materialized_cte.children[0]);
	refreshing = false;
}

void CTEFilterPusher::ClearDependencies(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		op.Cast<LogicalMaterializedCTE>().filter_dependency.reset();
	}
	for (auto &child : op.children) {
		ClearDependencies(*child);
	}
}

bool CTEFilterPusher::CanPushFilter(MaterializedCTEInfo &info) {
	if (info.AllReferencesAreFiltered()) {
		return true;
	}
	auto filters = info.Filters();
	if (filters.size() != 1 || !HasValidDependency(info)) {
		return false;
	}
	auto &cte = info.materialized_cte.Cast<LogicalMaterializedCTE>();
	D_ASSERT(filters[0].get().children[0]->type == LogicalOperatorType::LOGICAL_CTE_REF);
	auto &filtered_ref = filters[0].get().children[0]->Cast<LogicalCTERef>();
	return filtered_ref.table_index == cte.filter_dependency->row_scan;
}

bool CTEFilterPusher::HasValidDependency(const MaterializedCTEInfo &info) {
	auto &cte = info.materialized_cte.Cast<LogicalMaterializedCTE>();
	return cte.filter_dependency && cte.filter_dependency->MatchesConsumers(info.References());
}

void CTEFilterPusher::FindCandidates(LogicalOperator &op) {
	auto op_position = refreshing ? position : position++;
	if (op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE && !refreshing) {
		// We encountered a new CTE, add it to the map
		auto key = to_string(op.Cast<LogicalMaterializedCTE>().table_index.index);
		auto value = make_uniq<MaterializedCTEInfo>(op);
		auto &info = *value;
		cte_info_map.insert(key, std::move(value));
		info.definition_begin = position;
		FindCandidates(*op.children[0]);
		info.definition_end = position;
		FindCandidates(*op.children[1]);
		return;
	}
	if (op.type == LogicalOperatorType::LOGICAL_FILTER &&
	    op.children[0]->type == LogicalOperatorType::LOGICAL_CTE_REF) {
		// We encountered a filtered CTE ref, update the according CTE info
		auto &cte_ref = op.children[0]->Cast<LogicalCTERef>();
		auto it = cte_info_map.find(to_string(cte_ref.cte_index.index));
		if (it != cte_info_map.end()) {
			it->second->references.emplace(op_position, CTEReference {op, cte_ref});
		}
		return;
	}
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		// We encountered a CTE ref without a filter on top, so we can't do the optimization
		auto &cte_ref = op.Cast<LogicalCTERef>();
		auto it = cte_info_map.find(to_string(cte_ref.cte_index.index));
		if (it != cte_info_map.end()) {
			it->second->references.emplace(op_position, CTEReference {nullptr, cte_ref});
		}
		return;
	}
	for (auto &child : op.children) {
		FindCandidates(*child);
	}
}

void CTEFilterPusher::PushFilterIntoCTE(MaterializedCTEInfo &info) {
	D_ASSERT(info.materialized_cte.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE);
	auto filters = info.Filters();
	if (filters.empty()) {
		return;
	}

	// Create an OR expression with all the filters on all references of the CTE
	auto outer_expr = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR);
	for (auto &filter : filters) {
		D_ASSERT(filter.get().type == LogicalOperatorType::LOGICAL_FILTER);

		auto old_bindings = filter.get().children[0]->GetColumnBindings();
		auto new_bindings = info.materialized_cte.children[0]->GetColumnBindings();
		D_ASSERT(old_bindings.size() == new_bindings.size());

		ColumnBindingReplacer replacer;
		replacer.replacement_bindings.reserve(old_bindings.size());
		for (idx_t i = 0; i < old_bindings.size(); i++) {
			replacer.replacement_bindings.emplace_back(old_bindings[i], new_bindings[i]);
		}

		bool all_conjuncts_repeatable = true;
		for (auto &expr : filter.get().expressions) {
			all_conjuncts_repeatable &= !expr->IsVolatile() && !ExpressionBarrier::Contains(*expr);
		}

		// We copy the filters and replace the CTE reference bindings with the bindings in the CTE definition
		auto inner_expr = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		for (auto &filter_expr : filter.get().expressions) {
			// Dropping a conjunct must not expose errors it previously short-circuited.
			if (filter_expr->IsVolatile() || ExpressionBarrier::Contains(*filter_expr) ||
			    (!all_conjuncts_repeatable && filter_expr->CanThrow())) {
				continue;
			}
			auto filter_expr_copy = filter_expr->Copy();
			replacer.VisitExpression(&filter_expr_copy);
			inner_expr->GetChildrenMutable().push_back(std::move(filter_expr_copy));
		}

		// An unrestricted consumer makes the disjunction true.
		if (inner_expr->GetChildren().empty()) {
			return;
		}

		if (inner_expr->GetChildren().size() == 1) {
			outer_expr->GetChildrenMutable().push_back(std::move(inner_expr->GetChildrenMutable()[0]));
		} else {
			outer_expr->GetChildrenMutable().push_back(std::move(inner_expr));
		}
	}

	// Add the filter on top of the CTE definition and split the predicates
	unique_ptr<Expression> cte_filter;
	if (outer_expr->GetChildren().size() == 1) {
		cte_filter = std::move(outer_expr->GetChildrenMutable()[0]);
	} else {
		cte_filter = std::move(outer_expr);
	}
	auto new_cte = make_uniq_base<LogicalOperator, LogicalFilter>(std::move(cte_filter));
	LogicalFilter::SplitPredicates(new_cte->Cast<LogicalFilter>().expressions);

	// Rewrite the operator expressions before adding the child op (children should be rewritten already)
	optimizer.rewriter.VisitOperator(*new_cte);
	new_cte->children.push_back(std::move(info.materialized_cte.children[0]));

	// Push down the filter
	FilterPushdown pushdown(optimizer, true, FilterPushdown::ProjectionMode::PRESERVE_COMPUTED_EXPRESSIONS);
	new_cte = pushdown.Rewrite(std::move(new_cte));

	info.materialized_cte.children[0] = std::move(new_cte);
}

} // namespace duckdb
