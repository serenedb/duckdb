#include "duckdb/optimizer/late_materialization.hpp"

#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/optimizer/late_materialization_helper.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_explain.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_lookup_join.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_sample.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

LateMaterialization::LateMaterialization(Optimizer &optimizer) : optimizer(optimizer) {
	max_row_count = Settings::Get<LateMaterializationMaxRowsSetting>(optimizer.context);
	lookup_max_row_count = Settings::Get<LateMaterializationLookupMaxRowsSetting>(optimizer.context);
}

static void MapThroughProjectionMap(vector<ProjectionIndex> &projection_map, LogicalOperator &child,
                                    const vector<ColumnBinding> &bindings) {
	auto child_bindings = child.GetColumnBindings();
	for (auto &binding : bindings) {
		auto entry = std::find(child_bindings.begin(), child_bindings.end(), binding);
		D_ASSERT(entry != child_bindings.end());
		ProjectionIndex child_index(NumericCast<idx_t>(entry - child_bindings.begin()));
		if (std::find(projection_map.begin(), projection_map.end(), child_index) == projection_map.end()) {
			projection_map.push_back(child_index);
		}
	}
}

vector<ColumnBinding> LateMaterialization::ConstructRHS(unique_ptr<LogicalOperator> &op,
                                                        const vector<ProjectionIndex> &indexes) {
	// traverse down until we reach the LogicalGet
	vector<reference<LogicalOperator>> stack;
	reference<LogicalOperator> child = *op->children[0];
	while (child.get().type != LogicalOperatorType::LOGICAL_GET) {
		stack.push_back(child);
		D_ASSERT(child.get().children.size() == 1);
		child = *child.get().children[0];
	}
	auto &get = child.get().Cast<LogicalGet>();

	// project the columns up the stack
	vector<ColumnBinding> bindings;
	vector<Identifier> names;
	vector<LogicalType> types;
	for (auto &index : indexes) {
		auto &column_index = get.GetColumnIds()[index];
		bindings.emplace_back(get.table_index, index);
		names.push_back(get.GetColumnName(column_index));
		types.push_back(get.GetColumnType(column_index));
	}
	for (idx_t i = stack.size(); i > 0; i--) {
		auto &op = stack[i - 1].get();
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_PROJECTION: {
			auto &proj = op.Cast<LogicalProjection>();
			// push projection of the columns
			for (idx_t c = 0; c < bindings.size(); c++) {
				auto existing = std::find_if(proj.expressions.begin(), proj.expressions.end(), [&](const auto &expr) {
					return expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
					       expr->template Cast<BoundColumnRefExpression>().Binding() == bindings[c];
				});
				if (existing != proj.expressions.end()) {
					bindings[c] = ColumnBinding(
					    proj.table_index, ProjectionIndex(NumericCast<idx_t>(existing - proj.expressions.begin())));
					continue;
				}
				auto column_ref = make_uniq<BoundColumnRefExpression>(names[c], types[c], bindings[c]);
				bindings[c] = ColumnBinding(proj.table_index,
				                            ColumnBinding::PushExpression(proj.expressions, std::move(column_ref)));
			}
			break;
		}
		case LogicalOperatorType::LOGICAL_FILTER: {
			auto &filter = op.Cast<LogicalFilter>();
			// column bindings pass-through this operator as-is UNLESS the filter has a projection map
			if (filter.HasProjectionMap()) {
				// if the filter has a projection map, we need to project the new column
				MapThroughProjectionMap(filter.projection_map, *filter.children[0], bindings);
			}
			break;
		}
		default:
			throw InternalException("Unsupported logical operator in LateMaterialization::ConstructRHS");
		}
	}
	// The stack starts below the root, so the root's own projection map has not been
	// touched yet. A top-n carries one when the topn optimizer lifted its child
	// projection above it; the columns we just pushed are not in that map, so they
	// would be projected away again before the join can read them.
	if (op->type == LogicalOperatorType::LOGICAL_TOP_N) {
		auto &top_n = op->Cast<LogicalTopN>();
		if (top_n.HasProjectionMap()) {
			MapThroughProjectionMap(top_n.projection_map, *top_n.children[0], bindings);
		}
	}
	return bindings;
}

void LateMaterialization::ReplaceTopLevelTableIndex(LogicalOperator &root, TableIndex new_index) {
	reference<LogicalOperator> current_op = root;
	while (true) {
		auto &op = current_op.get();
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_PROJECTION: {
			// reached a projection - modify the table index and return
			auto &proj = op.Cast<LogicalProjection>();
			proj.table_index = new_index;
			return;
		}
		case LogicalOperatorType::LOGICAL_GET: {
			// reached the root get - modify the table index and return
			auto &get = op.Cast<LogicalGet>();
			get.table_index = new_index;
			return;
		}
		case LogicalOperatorType::LOGICAL_TOP_N: {
			// visit the expressions of the operator and continue into the child node
			auto &top_n = op.Cast<LogicalTopN>();
			for (auto &order : top_n.orders) {
				ReplaceTableReferences(order.expression, new_index);
			}
			current_op = *op.children[0];
			break;
		}
		case LogicalOperatorType::LOGICAL_FILTER:
		case LogicalOperatorType::LOGICAL_SAMPLE:
		case LogicalOperatorType::LOGICAL_LIMIT: {
			// visit the expressions of the operator and continue into the child node
			for (auto &expr : op.expressions) {
				ReplaceTableReferences(expr, new_index);
			}
			current_op = *op.children[0];
			break;
		}
		default:
			throw InternalException("Unsupported operator type in LateMaterialization::ReplaceTopLevelTableIndex");
		}
	}
}

void LateMaterialization::ReplaceTableReferences(unique_ptr<Expression> &root_expr, TableIndex new_table_index) {
	ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
	    root_expr, [&](BoundColumnRefExpression &bound_column_ref, unique_ptr<Expression> &expr) {
		    bound_column_ref.BindingMutable().table_index = new_table_index;
	    });
}

unique_ptr<Expression> LateMaterialization::GetExpression(LogicalOperator &op, ProjectionIndex column_index) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = op.Cast<LogicalGet>();
		ColumnBinding column_binding(get.table_index, column_index);
		auto &column_id = get.GetColumnIndex(column_binding);
		auto column_name = get.GetColumnName(column_id);
		auto &column_type = get.GetColumnType(column_id);
		auto expr = make_uniq<BoundColumnRefExpression>(Identifier(column_name), column_type, column_binding);
		return std::move(expr);
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &proj = op.Cast<LogicalProjection>();
		ColumnBinding column_binding(proj.table_index, column_index);
		auto &expr = proj.GetExpression(column_binding);
		return expr.Copy();
	}
	default:
		throw InternalException("Unsupported operator type for LateMaterialization::GetExpression");
	}
}

void LateMaterialization::ReplaceExpressionReferences(LogicalOperator &next_op, unique_ptr<Expression> &root_expr) {
	ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
	    root_expr, [&](BoundColumnRefExpression &bound_column_ref, unique_ptr<Expression> &expr) {
		    expr = GetExpression(next_op, bound_column_ref.Binding().column_index);
	    });
}

static vector<TableColumn> GetVirtualColumns(const LogicalGet &get, const vector<column_t> &column_ids) {
	if (column_ids.empty()) {
		throw InternalException("Row Id Columns must not be empty");
	}
	vector<TableColumn> result;
	for (auto &col_id : column_ids) {
		auto entry = get.virtual_columns.find(col_id);
		if (entry == get.virtual_columns.end()) {
			throw InternalException("Row id column id not found in virtual column list");
		}
		result.push_back(entry->second);
	}
	return result;
}

void LateMaterialization::LoadRowIdColumns(LogicalGet &get) {
	row_id_column_ids = get.function.get_row_id_columns(optimizer.context, get.bind_data.get());
	row_id_columns = GetVirtualColumns(get, row_id_column_ids);
}

static vector<unique_ptr<Expression>> RowIdReferences(const vector<TableColumn> &columns, const vector<string> &names,
                                                      const vector<ColumnBinding> &bindings) {
	vector<unique_ptr<Expression>> result;
	for (idx_t i = 0; i < columns.size(); i++) {
		auto name = i < names.size() ? Identifier(names[i]) : columns[i].name;
		result.push_back(make_uniq<BoundColumnRefExpression>(std::move(name), columns[i].type, bindings[i]));
	}
	return result;
}

vector<unique_ptr<Expression>>
LateMaterialization::ConstructFinalProjection(LogicalGet &lhs_get, idx_t lhs_columns,
                                              const vector<reference<LogicalOperator>> &source_operators) {
	vector<unique_ptr<Expression>> final_proj_list;
	// construct the final projection list from either (1) the root projection, or (2) the logical get
	if (!source_operators.empty()) {
		// construct the columns from the root projection
		auto &root_proj = source_operators[0].get();
		for (auto &expr : root_proj.expressions) {
			final_proj_list.push_back(expr->Copy());
		}
		// now we need to "flatten" the projection list by traversing the set of projections and inlining them
		for (idx_t i = 0; i < source_operators.size(); i++) {
			auto &next_operator = i + 1 < source_operators.size() ? source_operators[i + 1].get() : lhs_get;
			for (auto &expr : final_proj_list) {
				ReplaceExpressionReferences(next_operator, expr);
			}
		}
	} else {
		// if we have no projection directly construct the columns from the root get
		for (idx_t i = 0; i < lhs_columns; i++) {
			final_proj_list.push_back(GetExpression(lhs_get, ProjectionIndex(i)));
		}
	}
	return final_proj_list;
}

static bool ExtractsWithCast(const ColumnIndex &column_index) {
	if (!column_index.IsPushdownExtract()) {
		return false;
	}
	auto &child = column_index.GetChildIndex(0);
	if (child.IsPushdownExtract()) {
		return ExtractsWithCast(child);
	}
	auto &type = column_index.GetType();
	auto &field_type = child.HasPrimaryIndex() ? StructType::GetChildType(type, child.GetPrimaryIndex()) : type;
	return child.GetType() != field_type;
}

bool LateMaterialization::SupportsLookup(LogicalGet &get, TableFunctionLookupInfo &info) {
	if (!get.function.supports_lookup || !get.function.get_lookup_function || !get.function.get_row_id_columns) {
		return false;
	}
	for (auto &column_index : get.GetColumnIds()) {
		info.fetchable.push_back(!column_index.IsVirtualColumn() && !ExtractsWithCast(column_index));
	}
	info.from_source.resize(info.fetchable.size());
	return get.function.supports_lookup(optimizer.context, get, info);
}

bool LateMaterialization::TryLookupJoin(unique_ptr<LogicalOperator> &op, LogicalGet &get,
                                        const vector<reference<LogicalOperator>> &source_operators,
                                        const column_binding_set_t &required_columns,
                                        const TableFunctionLookupInfo &info) {
	const auto column_count = info.fetchable.size();
	vector<bool> fetched(column_count);
	vector<bool> from_source(column_count);
	bool index_lookup = false;
	bool source_lookup = false;
	bool key_resolved = false;
	for (idx_t i = 0; i < column_count; i++) {
		fetched[i] = info.fetchable[i] && !required_columns.count(ColumnBinding(get.table_index, ProjectionIndex(i)));
		from_source[i] = get.function.get_lookup_source && !info.source_row_id_columns.empty() && info.from_source[i];
		if (!from_source[i]) {
			index_lookup = index_lookup || fetched[i];
		} else if (fetched[i]) {
			source_lookup = true;
		} else {
			key_resolved = true;
		}
	}
	if (std::find(fetched.begin(), fetched.end(), true) == fetched.end()) {
		return false;
	}
	vector<ProjectionIndex> source_columns;
	unique_ptr<LogicalGet> lookup_source;
	if (source_lookup && (key_resolved || index_lookup || info.prefer_source_lookup)) {
		for (idx_t i = 0; i < column_count; i++) {
			if (fetched[i] && from_source[i]) {
				source_columns.emplace_back(i);
			}
		}
		lookup_source = get.function.get_lookup_source(optimizer.context, get, source_columns,
		                                               optimizer.binder.GenerateTableIndex());
	}
	if (!lookup_source) {
		std::fill(from_source.begin(), from_source.end(), false);
		index_lookup = true;
	} else if (!key_resolved) {
		index_lookup = true;
	}
	LoadRowIdColumns(get);

	auto source = LateMaterializationHelper::CreateLHSGet(get, optimizer.binder);
	source->table_filters.ClearFilters();
	auto source_index = source->table_index;
	auto final_proj_list = ConstructFinalProjection(*source, source->GetColumnIds().size(), source_operators);
	if (source_operators.empty() && !get.projection_ids.empty()) {
		for (idx_t i = 0; i < final_proj_list.size(); i++) {
			if (std::find(get.projection_ids.begin(), get.projection_ids.end(), ProjectionIndex(i)) ==
			    get.projection_ids.end()) {
				final_proj_list[i] = make_uniq<BoundConstantExpression>(Value(final_proj_list[i]->GetReturnType()));
			}
		}
	}

	vector<ProjectionIndex> carried;
	for (auto &expr : final_proj_list) {
		ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
		    *expr, [&](const BoundColumnRefExpression &colref) {
			    auto &binding = colref.Binding();
			    if (binding.table_index == source_index && !fetched[binding.column_index] &&
			        std::find(carried.begin(), carried.end(), binding.column_index) == carried.end()) {
				    carried.push_back(binding.column_index);
			    }
		    });
	}

	vector<ProjectionIndex> indexes;
	if (index_lookup) {
		indexes = LateMaterializationHelper::GetOrInsertRowIds(get, row_id_column_ids, row_id_columns);
	}
	vector<TableColumn> key_columns;
	if (lookup_source) {
		key_columns = GetVirtualColumns(get, info.source_row_id_columns);
	}
	if (lookup_source && key_resolved) {
		auto key_indexes = LateMaterializationHelper::GetOrInsertRowIds(get, info.source_row_id_columns, key_columns);
		indexes.insert(indexes.end(), key_indexes.begin(), key_indexes.end());
	}
	const auto carried_offset = indexes.size();
	indexes.insert(indexes.end(), carried.begin(), carried.end());

	auto rhs_bindings = ConstructRHS(op, indexes);
	auto final_index = rhs_bindings[0].table_index;
	auto rhs_table_index = optimizer.binder.GenerateTableIndex();
	for (auto &rhs_binding : rhs_bindings) {
		rhs_binding.table_index = rhs_table_index;
	}
	ReplaceTopLevelTableIndex(*op, rhs_table_index);

	for (auto &expr : final_proj_list) {
		ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
		    expr, [&](BoundColumnRefExpression &colref, unique_ptr<Expression> &) {
			    auto &binding = colref.BindingMutable();
			    if (binding.table_index != source_index) {
				    return;
			    }
			    if (!fetched[binding.column_index]) {
				    auto entry = std::find(carried.begin(), carried.end(), binding.column_index);
				    binding = rhs_bindings[carried_offset + NumericCast<idx_t>(entry - carried.begin())];
			    } else if (from_source[binding.column_index]) {
				    auto entry = std::find(source_columns.begin(), source_columns.end(), binding.column_index);
				    binding = ColumnBinding(lookup_source->table_index,
				                            ProjectionIndex(NumericCast<idx_t>(entry - source_columns.begin())));
			    }
		    });
	}

	const auto row_id_count = index_lookup ? row_id_columns.size() : 0;
	vector<ColumnBinding> key_bindings(rhs_bindings.begin() + NumericCast<int64_t>(row_id_count),
	                                   rhs_bindings.begin() + NumericCast<int64_t>(carried_offset));
	unique_ptr<LogicalOperator> join = std::move(op);
	if (index_lookup) {
		if (lookup_source && !key_resolved) {
			for (auto &index :
			     LateMaterializationHelper::GetOrInsertRowIds(*source, info.source_row_id_columns, key_columns)) {
				key_bindings.emplace_back(source_index, index);
			}
		}
		join = make_uniq<LogicalLookupJoin>(std::move(join), std::move(source),
		                                    RowIdReferences(row_id_columns, info.row_id_names, rhs_bindings));
	}
	if (lookup_source) {
		join = make_uniq<LogicalLookupJoin>(std::move(join), std::move(lookup_source),
		                                    RowIdReferences(key_columns, info.source_row_id_names, key_bindings));
	}
	auto proj = make_uniq<LogicalProjection>(final_index, std::move(final_proj_list));
	if (join->has_estimated_cardinality) {
		proj->SetEstimatedCardinality(join->estimated_cardinality);
	}
	proj->children.push_back(std::move(join));
	op = std::move(proj);

	RemoveUnusedColumns unused_optimizer(optimizer);
	unused_optimizer.VisitOperator(op);
	return true;
}

bool LateMaterialization::TryLateMaterialization(unique_ptr<LogicalOperator> &op, idx_t row_count, bool semi_join,
                                                 bool unfiltered_lookup) {
	// check if we can benefit from late materialization
	// we need to see how many columns we require in the pipeline versus how many columns we emit in the scan
	// for example, in a query like SELECT * FROM tbl ORDER BY ts LIMIT 5, the top-n only needs the "ts" column
	// the other columns can be fetched later on using late materialization
	// we can only push late materialization through a subset of operators
	// and we can only do it for scans that support the row-id pushdown (currently only DuckDB table scans)

	// visit the expressions for each operator in the chain
	vector<reference<LogicalOperator>> source_operators;
	bool filters_rows = false;

	column_references.clear();
	VisitOperatorExpressions(*op);
	reference<LogicalOperator> child = *op->children[0];
	while (child.get().type != LogicalOperatorType::LOGICAL_GET) {
		switch (child.get().type) {
		case LogicalOperatorType::LOGICAL_PROJECTION: {
			// recurse into the child node - but ONLY visit expressions that are referenced
			auto &proj = child.get().Cast<LogicalProjection>();
			source_operators.push_back(child);

			for (auto &expr : proj.expressions) {
				if (expr->IsVolatile()) {
					// we cannot do this optimization if any of the columns are volatile
					return false;
				}
			}

			// figure out which projection expressions we are currently referencing
			set<idx_t> referenced_columns;
			for (auto &entry : column_references) {
				auto &column_binding = entry.first;
				if (column_binding.table_index == proj.table_index) {
					referenced_columns.insert(column_binding.column_index);
				}
			}
			// clear the list of referenced expressions and visit those columns
			column_references.clear();
			for (auto &col_idx : referenced_columns) {
				VisitExpression(&proj.expressions[col_idx]);
			}
			// continue into child
			child = *child.get().children[0];
			break;
		}
		case LogicalOperatorType::LOGICAL_FILTER: {
			filters_rows = true;
			// visit filter expressions - we need these columns
			VisitOperatorExpressions(child.get());
			// continue into child
			child = *child.get().children[0];
			break;
		}
		default:
			// unsupported operator for late materialization
			return false;
		}
	}
	auto &get = child.get().Cast<LogicalGet>();
	column_binding_set_t required_columns;
	for (auto &entry : column_references) {
		required_columns.insert(entry.first);
	}
	for (auto &filter_entry : get.table_filters) {
		required_columns.insert(ColumnBinding(get.table_index, filter_entry.GetIndex()));
		filters_rows = true;
	}
	TableFunctionLookupInfo lookup_info;
	auto supports_lookup = SupportsLookup(get, lookup_info);
	if (lookup_info.applies_limit || (lookup_info.row_storage && !lookup_info.remote)) {
		return false;
	}
	if (lookups && supports_lookup && row_count <= lookup_max_row_count &&
	    (unfiltered_lookup || filters_rows || lookup_info.filters_rows) &&
	    TryLookupJoin(op, get, source_operators, required_columns, lookup_info)) {
		return true;
	}
	if (!semi_join || required_columns.size() >= get.GetColumnIds().size()) {
		// we do not benefit from late materialization
		// every column the scan reads is already required upstream (Top-N/Limit/etc) or by its pushed-down filters
		return false;
	}
	if (!get.function.late_materialization && (!supports_lookup || row_count <= lookup_max_row_count)) {
		// this function does not support late materialization
		return false;
	}
	if (get.extra_info.sample_options && !get.extra_info.sample_options->is_percentage) {
		// we should not apply late materialization when row-count sampling is pushed down
		// the sample scan is already fast and creating a semi-join would duplicate the full table scan
		return false;
	}
	if (!get.function.get_row_id_columns) {
		throw InternalException("Function supports late materialization but not get_row_id_columns");
	}
	LoadRowIdColumns(get);
	// we benefit from late materialization
	// we need to transform this plan into a semi-join with the row-id
	// we need to ensure the operator returns exactly the same column bindings as before

	// construct the LHS from the LogicalGet
	auto lhs = LateMaterializationHelper::CreateLHSGet(get, optimizer.binder);
	// insert the row-id column on the left hand side
	auto &lhs_get = *lhs;
	auto lhs_index = lhs_get.table_index;
	auto lhs_columns = lhs_get.GetColumnIds().size();
	auto lhs_row_indexes = LateMaterializationHelper::GetOrInsertRowIds(lhs_get, row_id_column_ids, row_id_columns);
	vector<ColumnBinding> lhs_bindings;
	for (auto &lhs_row_index : lhs_row_indexes) {
		lhs_bindings.emplace_back(lhs_index, lhs_row_index);
	}

	// after constructing the LHS but before constructing the RHS we construct the final projections/orders
	// - we do this before constructing the RHS because that alter the original plan
	auto final_proj_list = ConstructFinalProjection(lhs_get, lhs_columns, source_operators);

	// we need to re-order again at the end
	vector<BoundOrderByNode> final_orders;
	auto root_type = op->type;
	if (root_type == LogicalOperatorType::LOGICAL_TOP_N) {
		// for top-n we need to re-order by the top-n conditions
		auto &top_n = op->Cast<LogicalTopN>();
		for (auto &order : top_n.orders) {
			auto expr = order.expression->Copy();
			final_orders.emplace_back(order.type, order.null_order, std::move(expr));
		}
	} else {
		// for limit/sample we order by row-id
		for (idx_t r_idx = 0; r_idx < row_id_columns.size(); r_idx++) {
			auto &row_id_col = row_id_columns[r_idx];
			auto row_id_expr =
			    make_uniq<BoundColumnRefExpression>(row_id_col.name, row_id_col.type, lhs_bindings[r_idx]);
			final_orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_LAST, std::move(row_id_expr));
		}
	}

	// construct the RHS for the join
	// this is essentially the old pipeline, but with the `rowid` column added
	// note that the purpose of this optimization is to remove columns from the RHS
	// we don't do that here yet though - we do this in a later step using the RemoveUnusedColumns optimizer
	auto rhs_bindings =
	    ConstructRHS(op, LateMaterializationHelper::GetOrInsertRowIds(get, row_id_column_ids, row_id_columns));

	// the final table index emitted must be the table index of the original operator
	// this ensures any upstream operators that refer to the original get will keep on referring to the correct columns
	auto final_index = rhs_bindings[0].table_index;

	// we need to replace any references to "rhs_binding.table_index" in the rhs to a new table index
	auto rhs_table_index = optimizer.binder.GenerateTableIndex();
	for (auto &rhs_binding : rhs_bindings) {
		rhs_binding.table_index = rhs_table_index;
	}
	ReplaceTopLevelTableIndex(*op, rhs_table_index);
	if (rhs_bindings.size() != lhs_bindings.size()) {
		throw InternalException("Mismatch in late materialization binding sizes");
	}

	// construct a semi join between the lhs and rhs
	auto join = make_uniq<LogicalComparisonJoin>(JoinType::SEMI);
	join->children.push_back(std::move(lhs));
	join->children.push_back(std::move(op));

	for (idx_t r_idx = 0; r_idx < row_id_columns.size(); r_idx++) {
		auto &row_id_col = row_id_columns[r_idx];
		JoinCondition condition(
		    make_uniq<BoundColumnRefExpression>(row_id_col.name, row_id_col.type, lhs_bindings[r_idx]),
		    make_uniq<BoundColumnRefExpression>(row_id_col.name, row_id_col.type, rhs_bindings[r_idx]),
		    ExpressionType::COMPARE_EQUAL);
		join->conditions.push_back(std::move(condition));
	}

	// push a projection that removes the row id again from the lhs
	// this is the final projection - so it should have the final table index
	auto proj_index = final_index;
	if (root_type == LogicalOperatorType::LOGICAL_TOP_N) {
		// for top-n we need to order on expressions, so we need to order AFTER the final projection
		auto proj = make_uniq<LogicalProjection>(proj_index, std::move(final_proj_list));
		if (join->has_estimated_cardinality) {
			proj->SetEstimatedCardinality(join->estimated_cardinality);
		}
		proj->children.push_back(std::move(join));

		for (auto &order : final_orders) {
			ReplaceTableReferences(order.expression, proj_index);
		}
		auto order = make_uniq<LogicalOrder>(std::move(final_orders));
		if (proj->has_estimated_cardinality) {
			order->SetEstimatedCardinality(proj->estimated_cardinality);
		}
		order->children.push_back(std::move(proj));

		op = std::move(order);
	} else {
		// for limit/sample we order on row-id, so we need to order BEFORE the final projection
		// because the final projection removes row-ids
		auto order = make_uniq<LogicalOrder>(std::move(final_orders));
		if (join->has_estimated_cardinality) {
			order->SetEstimatedCardinality(join->estimated_cardinality);
		}
		order->children.push_back(std::move(join));

		auto proj = make_uniq<LogicalProjection>(proj_index, std::move(final_proj_list));
		if (order->has_estimated_cardinality) {
			proj->SetEstimatedCardinality(order->estimated_cardinality);
		}
		proj->children.push_back(std::move(order));

		op = std::move(proj);
	}

	// run the RemoveUnusedColumns optimizer to prune the (now) unused columns the plan
	RemoveUnusedColumns unused_optimizer(optimizer);
	unused_optimizer.VisitOperator(op);
	return true;
}

bool LateMaterialization::OptimizeLargeLimit(LogicalLimit &limit, idx_t limit_val, bool has_offset) {
	if (!has_offset && !Settings::Get<PreserveInsertionOrderSetting>(optimizer.context)) {
		// we avoid optimizing large limits if preserve insertion order is false
		// since the limit is executed in parallel anyway
		return false;
	}
	// we only perform this optimization until a certain amount of maximum values to reduce memory constraints
	// since we still materialize the set of row-ids in the hash table this optimization can increase memory pressure
	// FIXME: make this configurable as well
	static constexpr const idx_t LIMIT_MAX_VAL = 1000000;
	if (limit_val > LIMIT_MAX_VAL) {
		return false;
	}
	// we only support large limits if they are directly below the source
	reference<LogicalOperator> current_op = *limit.children[0];
	while (current_op.get().type != LogicalOperatorType::LOGICAL_GET) {
		if (current_op.get().type != LogicalOperatorType::LOGICAL_PROJECTION) {
			return false;
		}
		current_op = *current_op.get().children[0];
	}
	// if there are any filters we shouldn't do large limit optimization
	auto &get = current_op.get().Cast<LogicalGet>();
	if (!get.function.late_materialization || get.table_filters.HasFilters()) {
		return false;
	}
	return true;
}

unique_ptr<LogicalOperator> LateMaterialization::Optimize(unique_ptr<LogicalOperator> op) {
	switch (op->type) {
	case LogicalOperatorType::LOGICAL_LIMIT: {
		auto &limit = op->Cast<LogicalLimit>();
		if (limit.limit_val.Type() != LimitNodeType::CONSTANT_VALUE) {
			break;
		}
		auto limit_val = limit.limit_val.GetConstantValue();
		bool has_offset = limit.offset_val.Type() != LimitNodeType::UNSET;
		bool semi_join;
		if (limit_val > max_row_count) {
			// for large limits - we may still want to do this optimization if the limit is consecutive
			// this is the case if there are only projections/get below the limit
			// if the row-ids are not consecutive doing the join can worsen performance
			semi_join = OptimizeLargeLimit(limit, limit_val, has_offset);
		} else {
			// optimizing small limits really only makes sense if we have an offset
			semi_join = has_offset;
		}
		if (TryLateMaterialization(op, limit_val, semi_join, has_offset)) {
			return op;
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_TOP_N: {
		auto &top_n = op->Cast<LogicalTopN>();
		// for the top-n we need to visit the order elements
		if (TryLateMaterialization(op, top_n.limit, top_n.limit <= max_row_count, true)) {
			return op;
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_SAMPLE: {
		auto &sample = op->Cast<LogicalSample>();
		if (sample.sample_options->is_percentage) {
			break;
		}
		auto sample_size = sample.sample_options->sample_size.GetValue<uint64_t>();
		if (TryLateMaterialization(op, sample_size, sample_size <= max_row_count, true)) {
			return op;
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_EXPLAIN:
		lookups = op->Cast<LogicalExplain>().explain_type != ExplainType::EXPLAIN_SQL;
		break;
	default:
		break;
	}
	for (auto &child : op->children) {
		child = Optimize(std::move(child));
	}
	return op;
}

} // namespace duckdb
