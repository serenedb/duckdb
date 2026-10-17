#include "duckdb/planner/expression_binder/select_binder.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"

namespace duckdb {

SelectBinder::SelectBinder(Binder &binder, ClientContext &context, BoundSelectNode &node)
    : BaseSelectBinder(binder, context, node) {
}

bool SelectBinder::TryResolveAliasReference(ColumnRefExpression &colref, idx_t depth, bool root_expression,
                                            BindResult &result, unique_ptr<ParsedExpression> &expr_ptr) {
	// must be a qualified alias.<name>
	if (!ExpressionBinder::IsPotentialAlias(colref)) {
		return false;
	}

	const auto &alias_name = colref.ColumnNames().back();
	auto entry = node.bind_state.alias_map.find(alias_name);
	if (entry == node.bind_state.alias_map.end()) {
		return false;
	}

	auto alias_index = entry->second;
	// Simple way to prevent circular aliasing (`SELECT alias.y as x, alias.x as y;`)
	if (alias_index >= node.bound_column_count) {
		throw BinderException("Column %s referenced that exists in the SELECT clause - but this column "
		                      "cannot be referenced before it is defined",
		                      colref.ColumnNames().back());
	}

	if (depth == 0 && node.lateral_aliases_by_value && !inside_window && !inside_aggregate &&
	    !inside_aggregate_filter && !node.bind_state.IsExpandedColumn(alias_index)) {
		if (!node.lateral_index.IsValid()) {
			node.lateral_index = binder.GenerateTableIndex();
		}
		auto column = node.bind_state.GetFinalIndex(alias_index);
		result = BindResult(make_uniq<BoundColumnRefExpression>(colref.GetColumnName(),
		                                                        node.select_list[column]->GetReturnType(),
		                                                        ColumnBinding(node.lateral_index, column)));
		return true;
	}
	if (node.bind_state.AliasHasSubquery(alias_index)) {
		throw BinderException(colref,
		                      "Alias %s referenced in a SELECT clause - but the expression has a subquery. This is "
		                      "not yet supported.",
		                      alias_name);
	}
	auto copied_unbound = node.bind_state.BindAlias(alias_index);
	result = BindExpression(copied_unbound, depth, false);
	return true;
}

} // namespace duckdb
