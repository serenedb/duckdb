#include "duckdb/execution/column_binding_resolver.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_create_index.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"

namespace duckdb {

ColumnBindingResolver::ColumnBindingResolver(bool verify_only) : verify_only(verify_only) {
}

ColumnBindingResolver::ColumnBindingResolver(vector<ColumnBinding> bindings_p, vector<LogicalType> types_p)
    : bindings(std::move(bindings_p)), types(std::move(types_p)), verify_only(false) {
}

//! Combine the types of both join sides for resolving expressions against the combined bindings.
//! An empty type vector for a non-empty binding set means type verification was skipped for that side (e.g., legacy
//! extension operators), in which case we skip it for the combined set too.
static vector<LogicalType> CombineJoinTypes(const vector<ColumnBinding> &left_bindings,
                                            const vector<LogicalType> &left_types,
                                            const vector<ColumnBinding> &right_bindings,
                                            const vector<LogicalType> &right_types) {
	if ((left_types.empty() && !left_bindings.empty()) || (right_types.empty() && !right_bindings.empty())) {
		return vector<LogicalType>();
	}
	auto result = left_types;
	result.insert(result.end(), right_types.begin(), right_types.end());
	return result;
}

void ColumnBindingResolver::VisitOperator(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &comp_join = op.Cast<LogicalComparisonJoin>();

		VisitOperator(*comp_join.children[0]);
		auto left_bindings = bindings;
		auto left_types = types;
		for (auto &cond : comp_join.conditions) {
			if (cond.IsComparison()) {
				VisitExpression(&cond.LeftReference());
			}
		}

		for (auto &expr : comp_join.duplicate_eliminated_columns) {
			VisitExpression(&expr);
		}

		VisitOperator(*comp_join.children[1]);
		auto right_bindings = bindings;
		auto right_types = types;
		for (auto &cond : comp_join.conditions) {
			if (cond.IsComparison()) {
				VisitExpression(&cond.RightReference());
			}
		}

		// combine bindings to resolve predicate
		auto combined_bindings = left_bindings;
		combined_bindings.insert(combined_bindings.end(), right_bindings.begin(), right_bindings.end());
		auto combined_types = CombineJoinTypes(left_bindings, left_types, right_bindings, right_types);

		SetBindings(std::move(combined_bindings), std::move(combined_types));
		for (auto &cond : comp_join.conditions) {
			if (!cond.IsComparison()) {
				VisitExpression(&cond.JoinExpressionReference());
			}
		}

		// update to join output bindings
		SetBindings(op.GetColumnBindings(), op.types);

		return;
	}
	case LogicalOperatorType::LOGICAL_DELIM_JOIN: {
		auto &comp_join = op.Cast<LogicalComparisonJoin>();
		// get bindings from the duplicate-eliminated side
		auto &delim_side = comp_join.delim_flipped ? *comp_join.children[1] : *comp_join.children[0];
		VisitOperator(delim_side);
		auto delim_bindings = bindings;
		auto delim_types = types;
		for (auto &cond : comp_join.conditions) {
			if (cond.IsComparison()) {
				auto &expr = comp_join.delim_flipped ? cond.RightReference() : cond.LeftReference();
				VisitExpression(&expr);
			}
		}
		// visit the duplicate eliminated columns
		for (auto &expr : comp_join.duplicate_eliminated_columns) {
			VisitExpression(&expr);
		}
		// now the other side
		auto &other_side = comp_join.delim_flipped ? *comp_join.children[0] : *comp_join.children[1];
		VisitOperator(other_side);
		auto other_bindings = bindings;
		auto other_types = types;
		for (auto &cond : comp_join.conditions) {
			if (cond.IsComparison()) {
				auto &expr = comp_join.delim_flipped ? cond.LeftReference() : cond.RightReference();
				VisitExpression(&expr);
			}
		}

		// arbitrary expressions are resolved against both join sides in logical left/right order
		auto &left_bindings = comp_join.delim_flipped ? other_bindings : delim_bindings;
		auto &right_bindings = comp_join.delim_flipped ? delim_bindings : other_bindings;
		auto combined_bindings = left_bindings;
		combined_bindings.insert(combined_bindings.end(), right_bindings.begin(), right_bindings.end());
		auto &left_types = comp_join.delim_flipped ? other_types : delim_types;
		auto &right_types = comp_join.delim_flipped ? delim_types : other_types;
		auto combined_types = CombineJoinTypes(left_bindings, left_types, right_bindings, right_types);
		SetBindings(std::move(combined_bindings), std::move(combined_types));
		for (auto &cond : comp_join.conditions) {
			if (!cond.IsComparison()) {
				VisitExpression(&cond.JoinExpressionReference());
			}
		}
		// finally update the bindings with the result bindings of the join
		SetBindings(op.GetColumnBindings(), op.types);
		return;
	}
	case LogicalOperatorType::LOGICAL_LOOKUP_JOIN: {
		VisitOperator(*op.children[0]);
		VisitOperatorExpressions(op);
		VisitOperator(*op.children[1]);
		SetBindings(op.GetColumnBindings(), op.types);
		return;
	}
	case LogicalOperatorType::LOGICAL_ANY_JOIN: {
		// ANY join, this join is different because we evaluate the expression on the bindings of BOTH join sides at
		// once i.e. we set the bindings first to the bindings of the entire join, and then resolve the expressions of
		// this operator
		VisitOperatorChildren(op);
		auto any_join_bindings = op.GetColumnBindings();
		auto any_join_types = op.types;
		auto &any_join = op.Cast<LogicalAnyJoin>();
		if (any_join.join_type == JoinType::SEMI || any_join.join_type == JoinType::ANTI) {
			auto right_bindings = op.children[1]->GetColumnBindings();
			any_join_bindings.insert(any_join_bindings.end(), right_bindings.begin(), right_bindings.end());
			auto &right_types = op.children[1]->types;
			any_join_types.insert(any_join_types.end(), right_types.begin(), right_types.end());
		}
		SetBindings(std::move(any_join_bindings), std::move(any_join_types));
		if (any_join.join_type == JoinType::RIGHT_SEMI || any_join.join_type == JoinType::RIGHT_ANTI) {
			throw InternalException("RIGHT SEMI/ANTI any join not supported yet");
		}
		VisitOperatorExpressions(op);

		//	Restore bindings for the caller
		SetBindings(op.GetColumnBindings(), op.types);
		return;
	}
	case LogicalOperatorType::LOGICAL_CREATE_INDEX: {
		// Resolve children first; index expressions then reference (TableIndex(0), i).
		auto &create_index = op.Cast<LogicalCreateIndex>();
		VisitOperatorChildren(op);
		idx_t logical_column_count;
		if (create_index.table.type == CatalogType::VIEW_ENTRY) {
			auto &view = create_index.table.Cast<ViewCatalogEntry>();
			auto column_info = view.GetColumnInfo();
			logical_column_count = column_info ? column_info->types.size() : 0;
		} else {
			logical_column_count = create_index.table.Cast<TableCatalogEntry>().GetColumns().LogicalColumnCount();
		}
		// TODO: fill types in too (clearing skips type checks)
		SetBindings(LogicalOperator::GenerateColumnBindings(TableIndex(0), logical_column_count), {});
		VisitOperatorExpressions(op);
		return;
	}
	case LogicalOperatorType::LOGICAL_GET: {
		//! We first need to update the current set of bindings and then visit operator expressions
		SetBindings(op.GetColumnBindings(), op.types);
		VisitOperatorExpressions(op);
		return;
	}
	case LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR: {
		auto &ext_op = op.Cast<LogicalExtensionOperator>();
		if (ext_op.GetTypeBindingVerificationIdentifier()) {
			SetBindings({}, {});
			VisitOperatorChildren(op);
			VisitOperatorExpressions(op);
			SetBindings(op.GetColumnBindings(), op.types);
			return;
		}
		// Just to be very sure, we clear before and after resolving extension operator column bindings
		// This skips checks, but makes sure we don't break any extension operators with type verification
		types.clear();
		external_binding_updates++;
		ext_op.ResolveColumnBindings(*this, bindings);
		external_binding_updates--;
		binding_positions_valid = false;
		types.clear();
		return;
	}
	case LogicalOperatorType::LOGICAL_RECURSIVE_CTE: {
		VisitOperatorChildren(op);
		SetBindings(op.GetColumnBindings(), {});
		VisitOperatorExpressions(op);
		types = op.types;
		return;
	}
	default:
		break;
	}

	// general case
	// first visit the children of this operator
	VisitOperatorChildren(op);
	// now visit the expressions of this operator to resolve any bound column references
	VisitOperatorExpressions(op);
	// finally update the current set of bindings to the current set of column bindings
	SetBindings(op.GetColumnBindings(), op.types);
}

void ColumnBindingResolver::SetBindings(vector<ColumnBinding> new_bindings, vector<LogicalType> new_types) {
	bindings = std::move(new_bindings);
	types = std::move(new_types);
	binding_positions_valid = false;
}

optional_idx ColumnBindingResolver::FindBinding(ColumnBinding binding) {
	static constexpr idx_t LINEAR_SCAN_BINDINGS = 16;
	if (bindings.size() <= LINEAR_SCAN_BINDINGS || external_binding_updates > 0) {
		for (idx_t i = 0; i < bindings.size(); i++) {
			if (bindings[i] == binding) {
				return i;
			}
		}
		return optional_idx();
	}
	if (!binding_positions_valid) {
		binding_positions.clear();
		binding_positions.reserve(bindings.size());
		for (idx_t i = 0; i < bindings.size(); i++) {
			binding_positions.emplace(bindings[i], i);
		}
		binding_positions_valid = true;
	}
	auto entry = binding_positions.find(binding);
	if (entry == binding_positions.end()) {
		return optional_idx();
	}
	return entry->second;
}

unique_ptr<Expression> ColumnBindingResolver::VisitReplace(BoundColumnRefExpression &expr,
                                                           unique_ptr<Expression> *expr_ptr) {
	D_ASSERT(expr.Depth() == 0);
	// check the current set of column bindings to see which index corresponds to the column reference
	auto position = FindBinding(expr.Binding());
	if (position.IsValid()) {
		auto i = position.GetIndex();
		if (!types.empty()) {
			// LCOV_EXCL_START
			if (bindings.size() != types.size()) {
				throw InternalException(
				    "Failed to bind column reference %s [%d.%d]: inequal num bindings/types (%llu != %llu)",
				    expr.GetAlias(), expr.Binding().table_index.index, expr.Binding().column_index, bindings.size(),
				    types.size());
			}
			if (expr.GetReturnType() != types[i]) {
				throw InternalException("Failed to bind column reference %s [%d.%d]: inequal types (%s != %s)",
				                        expr.GetAlias(), expr.Binding().table_index.index, expr.Binding().column_index,
				                        expr.GetReturnType().ToString(), types[i].ToString());
			}
			// LCOV_EXCL_STOP
		}
		if (verify_only) {
			// in verification mode
			return nullptr;
		}
		return make_uniq<BoundReferenceExpression>(expr.GetAlias(), expr.GetReturnType(), i);
	}
	// LCOV_EXCL_START
	// could not bind the column reference, this should never happen and indicates a bug in the code
	// generate an error message
	throw InternalException("Failed to bind column reference %s [%d.%d] (bindings: %s)", expr.GetAlias(),
	                        expr.Binding().table_index.index, expr.Binding().column_index,
	                        LogicalOperator::ColumnBindingsToString(bindings));
	// LCOV_EXCL_STOP
}

} // namespace duckdb
