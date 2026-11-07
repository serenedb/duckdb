#include "duckdb/optimizer/cte_inlining.hpp"

#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"

#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/logical_operator_deep_copy.hpp"
#include "duckdb/planner/operator/logical_prepare.hpp"

#include "duckdb/function/scalar/generic_functions.hpp"

namespace duckdb {

CTEInlining::CTEInlining(Optimizer &optimizer_p) : optimizer(optimizer_p) {
}

unique_ptr<LogicalOperator> CTEInlining::Optimize(unique_ptr<LogicalOperator> op) {
	if (!RegisterScans(op)) {
		return op;
	}
	op->ResolveOperatorTypes();
	TryInlining(op, false);
	return op;
}

void CTEInlining::SubtreeInfo::Add(const SubtreeInfo &other, idx_t times) {
	has_side_effects |= other.has_side_effects;
	contains_delim_get |= other.contains_delim_get;
	prevents_inlining |= other.prevents_inlining;
	base_table_references += other.base_table_references * times;
}

bool CTEInlining::RegisterScans(unique_ptr<LogicalOperator> &op) {
	auto has_cte = op->type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE;
	if (op->type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto scan_id = scans.size();
		scans.push_back(CTEScan {op.get(), op});
		scan_index.emplace(op.get(), scan_id);
		cte_scans[op->Cast<LogicalCTERef>().cte_index].push_back(scan_id);
	}
	for (auto &child : op->children) {
		has_cte |= RegisterScans(child);
	}
	return has_cte;
}

void CTEInlining::UnregisterScans(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto entry = scan_index.find(&op);
		D_ASSERT(entry != scan_index.end());
		scans[entry->second].op = nullptr;
		scan_index.erase(entry);
	}
	for (auto &child : op.children) {
		UnregisterScans(*child);
	}
}

void CTEInlining::MoveScan(unique_ptr<LogicalOperator> &slot) {
	if (slot->type != LogicalOperatorType::LOGICAL_CTE_REF) {
		return;
	}
	auto entry = scan_index.find(slot.get());
	D_ASSERT(entry != scan_index.end());
	scans[entry->second].slot = slot;
}

static idx_t CountBaseTableReferences(const LogicalOperator &op) {
	idx_t number_of_references = 0;
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		number_of_references++;
	}
	for (auto &child : op.children) {
		number_of_references += CountBaseTableReferences(*child);
	}

	return number_of_references;
}

static idx_t CountCTEReferences(const LogicalOperator &op, TableIndex cte_index) {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &cte = op.Cast<LogicalCTERef>();
		if (cte.cte_index == cte_index) {
			return 1;
		}
	}
	idx_t number_of_references = 0;
	for (auto &child : op.children) {
		number_of_references += CountCTEReferences(*child, cte_index);
	}

	return number_of_references;
}

static bool ContainsLimit(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_LIMIT || op.type == LogicalOperatorType::LOGICAL_TOP_N) {
		return true;
	}
	if (op.children.size() != 1) {
		return false;
	}
	for (auto &child : op.children) {
		if (ContainsLimit(*child)) {
			return true;
		}
	}
	return false;
}

bool CTEInlining::EndsInAggregateOrDistinct(const LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
	case LogicalOperatorType::LOGICAL_DISTINCT:
	case LogicalOperatorType::LOGICAL_WINDOW:
		return true;
	default:
		break;
	}
	if (op.children.size() != 1) {
		return false;
	}
	for (auto &child : op.children) {
		if (EndsInAggregateOrDistinct(*child)) {
			return true;
		}
	}
	return false;
}

static bool EndsInDummyScan(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_DUMMY_SCAN || op.type == LogicalOperatorType::LOGICAL_EMPTY_RESULT ||
	    op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		return true;
	}
	if (op.children.size() != 1) {
		return false;
	}
	for (auto &child : op.children) {
		if (EndsInDummyScan(*child)) {
			return true;
		}
	}
	return false;
}

static bool ContainsDelimGet(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_DELIM_GET) {
		return true;
	}
	for (auto &child : op.children) {
		if (ContainsDelimGet(*child)) {
			return true;
		}
	}
	return false;
}

static bool HasCTEReferenceBelowDelimJoin(const LogicalOperator &op, TableIndex cte_index,
                                          bool below_delim_join = false) {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &cteref = op.Cast<LogicalCTERef>();
		if (cteref.cte_index == cte_index) {
			return below_delim_join;
		}
	}
	auto child_below_delim_join = below_delim_join || op.type == LogicalOperatorType::LOGICAL_DELIM_JOIN;
	for (auto &child : op.children) {
		if (HasCTEReferenceBelowDelimJoin(*child, cte_index, child_below_delim_join)) {
			return true;
		}
	}
	return false;
}

static bool PreventsInlining(LogicalOperator &op) {
	PreventInlining prevent_inlining;
	prevent_inlining.VisitOperator(op);
	return prevent_inlining.prevent_inlining;
}

CTEInlining::SubtreeInfo CTEInlining::TryInlining(unique_ptr<LogicalOperator> &op, bool collect_volatility) {
	if (op->type == LogicalOperatorType::LOGICAL_PREPARE) {
		// we are in a prepare statement, if we have to copy an operator during inlining,
		// we have to be careful to use the correct parameter data
		auto &prepare = op->Cast<LogicalPrepare>();
		parameter_data = prepare.prepared->value_map;
	}

	// traverse children first, so we can inline the deepest CTEs first
	if (op->type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		auto entry = cte_scans.find(op->Cast<LogicalMaterializedCTE>().table_index);
		auto collect_definition_volatility =
		    collect_volatility || (entry != cte_scans.end() && entry->second.size() > 1);
		auto definition = TryInlining(op->children[0], collect_definition_volatility);
		auto body = TryInlining(op->children[1], collect_volatility);
		return TryInliningCTE(op, definition, body, collect_definition_volatility);
	}
	SubtreeInfo info;
	for (auto &child : op->children) {
		info.Add(TryInlining(child, collect_volatility));
	}
	info.has_side_effects |= op->HasOwnSideEffects();
	info.contains_delim_get |= op->type == LogicalOperatorType::LOGICAL_DELIM_GET;
	if (op->type == LogicalOperatorType::LOGICAL_GET) {
		info.base_table_references++;
	}
	if (collect_volatility && !info.prevents_inlining) {
		PreventInlining prevent_inlining;
		LogicalOperatorVisitor::EnumerateExpressions(
		    *op, [&](unique_ptr<Expression> *expression) { prevent_inlining.VisitExpression(expression); });
		info.prevents_inlining = prevent_inlining.prevent_inlining;
	}
	return info;
}

CTEInlining::SubtreeInfo CTEInlining::TryInliningCTE(unique_ptr<LogicalOperator> &op, const SubtreeInfo &definition,
                                                     const SubtreeInfo &body, bool knows_volatility) {
	auto &cte = op->Cast<LogicalMaterializedCTE>();
	vector<idx_t> scan_ids;
	auto entry = cte_scans.find(cte.table_index);
	if (entry != cte_scans.end()) {
		scan_ids = std::move(entry->second);
		cte_scans.erase(entry);
	}
	idx_t ref_count = 0;
	for (auto scan_id : scan_ids) {
		if (scans[scan_id].op) {
			ref_count++;
		}
	}
	D_ASSERT_V(ref_count == CountCTEReferences(*op, cte.table_index));
	D_ASSERT_V(definition.has_side_effects == cte.children[0]->HasSideEffects());
	D_ASSERT_V(definition.contains_delim_get == ContainsDelimGet(*cte.children[0]));
	D_ASSERT_V(definition.base_table_references == CountBaseTableReferences(*cte.children[0]));
	D_ASSERT_V(!knows_volatility || definition.prevents_inlining == PreventsInlining(*cte.children[0]));

	auto kept = body;
	kept.Add(definition);
	if (definition.has_side_effects) {
		// Side-effecting CTEs must always execute even when unreferenced. Never inline them either: the
		// LOGICAL_MATERIALIZED_CTE guarantees that it executes exactly once and before the query side.
		return kept;
	}
	if (ref_count == 0) {
		// this CTE is not referenced, we can remove it
		RemoveCTE(op, false);
		return body;
	}
	if (definition.contains_delim_get && HasCTEReferenceBelowDelimJoin(*op->children[1], cte.table_index)) {
		// Inlining a CTE that already contains a DELIM_GET stays safe while all matching CTE scans remain outside
		// DELIM_JOIN subtrees, but once a scan is nested below another DELIM_JOIN the inlined DELIM_GETs can attach
		// to the wrong duplicate-elimination source.
		return kept;
	}
	if (cte.materialize == CTEMaterialize::CTE_MATERIALIZE_ALWAYS) {
		// This CTE is always materialized, we cannot inline it
		return kept;
	}
	if (ref_count == 1) {
		// this CTE is only referenced once, we can inline it directly without copying
		Inline(cte, scan_ids, false);
		RemoveCTE(op, true);
		return kept;
	}
	auto inline_copies = [&] {
		auto copies = Inline(cte, scan_ids, true);
		auto result = body;
		if (copies < ref_count) {
			result.Add(definition, copies + 1);
			return result;
		}
		RemoveCTE(op, false);
		result.Add(definition, copies);
		return result;
	};
	if (cte.materialize == CTEMaterialize::CTE_MATERIALIZE_NEVER) {
		// this CTE is referenced multiple times, but we are not allowed to materialize it
		// we have to inline it if possible
		return inline_copies();
	}
	// check if we can inline this CTE
	if (knows_volatility ? definition.prevents_inlining : PreventsInlining(*cte.children[0])) {
		// we cannot inline this CTE, we have to keep it materialized
		return kept;
	}

	// Prevent inlining if the CTE ends in an aggregate or distinct operator
	// This mimics the behavior of the CTE materialization in the binder
	if (EndsInAggregateOrDistinct(*op->children[0])) {
		return kept;
	}

	bool is_cheap_to_inline = op->children[0]->type == LogicalOperatorType::LOGICAL_EMPTY_RESULT ||
	                          op->children[0]->type == LogicalOperatorType::LOGICAL_CTE_REF ||
	                          EndsInDummyScan(*op->children[0]);

	// Check how many base table references the CTE has
	auto base_table_references = definition.base_table_references;

	if (!is_cheap_to_inline && base_table_references > 2 && base_table_references * ref_count > 10) {
		return kept;
	}

	// CTEs require full materialization before the CTE scans begin,
	// LIMIT and TOP_N operators cannot abort the materialization,
	// even if only a part of the CTE result is needed.
	// Therefore, we check if the CTE Scans are below the LIMIT or TOP_N operator
	// and if so, we try to inline the CTE definition.
	if (is_cheap_to_inline || ContainsLimit(*op->children[1])) {
		// this CTE is referenced multiple times and has a limit, we want to inline it
		return inline_copies();
	}
	return kept;
}

void CTEInlining::RemoveCTE(unique_ptr<LogicalOperator> &op, bool definition_moved) {
	if (!definition_moved) {
		UnregisterScans(*op->children[0]);
	}
	op = std::move(op->children[1]);
	MoveScan(op);
}

idx_t CTEInlining::Inline(LogicalOperator &materialized_cte, const vector<idx_t> &scan_ids, bool requires_copy) {
	auto &definition = materialized_cte.children[0];
	idx_t inlined = 0;
	for (auto scan_id : scan_ids) {
		if (!scans[scan_id].op) {
			continue;
		}
		auto &slot = scans[scan_id].slot.get();
		D_ASSERT(slot.get() == scans[scan_id].op.get());
		auto &cteref = slot->Cast<LogicalCTERef>();
		unique_ptr<LogicalProjection> projection;
		if (requires_copy) {
			// there are multiple references to the CTE, we need to copy it
			LogicalOperatorDeepCopy deep_copy(optimizer.binder, parameter_data);
			unique_ptr<LogicalOperator> copy;
			try {
				copy = deep_copy.DeepCopy(definition);
			} catch (NotImplementedException &ex) {
				// if we have to copy the lhs of a CTE, but we cannot copy the operator, we have to
				// stop inlining and keep the materialized CTE instead
				continue;
			}
			copy->ResolveOperatorTypes();
			projection = LogicalProjection::CreateResolvedIdentity(cteref.table_index, std::move(copy));
			RegisterScans(projection->children[0]);
		} else {
			projection = LogicalProjection::CreateResolvedIdentity(cteref.table_index, std::move(definition));
			MoveScan(projection->children[0]);
		}
		D_ASSERT(projection->children[0]->types == cteref.chunk_types);
		scans[scan_id].op = nullptr;
		scan_index.erase(slot.get());
		slot = std::move(projection);
		inlined++;
	}
	return inlined;
}

void PreventInlining::VisitOperator(LogicalOperator &op) {
	VisitOperatorExpressions(op);
	// We can stop checking early if we already know that inlining is not possible
	if (!prevent_inlining) {
		VisitOperatorChildren(op);
	}
}

void PreventInlining::VisitExpression(unique_ptr<Expression> *expression) {
	auto &expr = *expression;

	if (expr->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &bound_function = expr->Cast<BoundFunctionExpression>();
		// if we encounter the ErrorFun function, we still want to inline
		if (bound_function.Function().GetName() != "error" && expr->IsVolatile()) {
			prevent_inlining = true;
		}
		return;
	}
	VisitExpressionChildren(**expression);
}

} // namespace duckdb
