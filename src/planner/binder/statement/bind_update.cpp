#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/tableref.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/tableref/bound_joinref.hpp"
#include "duckdb/planner/constraints/bound_check_constraint.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_default_expression.hpp"
#include "duckdb/planner/expression_binder/update_binder.hpp"
#include "duckdb/planner/expression_binder/where_binder.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/storage/data_table.hpp"

#include <absl/algorithm/container.h>

namespace duckdb {

void Binder::ExpandStoredGeneratedExpression(unique_ptr<ParsedExpression> &expr, TableCatalogEntry &table) {
	ExpandStoredGeneratedExpression(expr, table, [](const Identifier &) { return unique_ptr<ParsedExpression>(); });
}

void Binder::ExpandStoredGeneratedExpression(
    unique_ptr<ParsedExpression> &expr, TableCatalogEntry &table,
    absl::FunctionRef<unique_ptr<ParsedExpression>(const Identifier &)> substitute) {
	if (expr->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
			ExpandStoredGeneratedExpression(child, table, substitute);
		});
		return;
	}
	const auto &name = expr->Cast<ColumnRefExpression>().GetColumnName();
	if (auto replacement = substitute(name)) {
		expr = std::move(replacement);
		return;
	}
	// A reference to another generated column is inlined and expanded, so chains resolve here.
	if (table.ColumnExists(name) && table.GetColumn(name).Generated()) {
		expr = table.GetColumn(name).GeneratedExpression().Copy();
		ExpandStoredGeneratedExpression(expr, table, substitute);
	}
}

void Binder::BindUpdateSet(TableIndex proj_index, unique_ptr<LogicalOperator> &root, UpdateSetInfo &set_info,
                           TableCatalogEntry &table, vector<PhysicalIndex> &columns,
                           const vector<unique_ptr<Expression>> &bound_defaults,
                           vector<unique_ptr<Expression>> &update_expressions,
                           vector<unique_ptr<Expression>> &projection_expressions, bool prioritize_table_when_binding) {
	D_ASSERT(set_info.columns.size() == set_info.expressions.size());

	Binder *expr_binder_ptr = this;
	shared_ptr<Binder> binder_with_search_path;

	if (prioritize_table_when_binding) {
		binder_with_search_path =
		    CreateBinderWithSearchPath(table.ParentCatalog().GetName(), table.ParentSchema().name);
		expr_binder_ptr = binder_with_search_path.get();
	}

	auto &all_columns = table.GetColumns();

	// Bind one column assignment and add it to the UPDATE projection. Shared by
	// the explicit SET list and the stored-generated recompute below so the two
	// paths stay in lockstep.
	auto append_assignment = [&](const ColumnDefinition &column, unique_ptr<ParsedExpression> &expr) {
		columns.push_back(column.Physical());
		if (expr->GetExpressionType() == ExpressionType::VALUE_DEFAULT) {
			auto bound_default = bound_defaults[column.StorageOid()]->Copy();
			auto expr_index = ColumnBinding::PushExpression(projection_expressions, std::move(bound_default));
			update_expressions.push_back(
			    make_uniq<BoundColumnRefExpression>(column.Type(), ColumnBinding(proj_index, expr_index)));
			return;
		}
		UpdateBinder binder(*expr_binder_ptr, context);
		binder.target_type = table.GetExpectedTypeForInsert(column);
		auto bound_expr = binder.Bind(expr);
		if (root) {
			PlanSubqueries(bound_expr, root);
		}
		auto bound_type = bound_expr->GetReturnType();
		auto expr_index = ColumnBinding::PushExpression(projection_expressions, std::move(bound_expr));
		auto source_binding = ColumnBinding(proj_index, expr_index);
		update_expressions.push_back(table.GetDefaultExpressionForColumn(
		    context, bound_type, column.Type(), source_binding, *bound_defaults[column.StorageOid()]));
	};

	// Stored generated columns are recomputed below when an UPDATE assigns a
	// column they derive from; with none present, skip all of that work.
	const bool has_stored_generated = absl::c_any_of(all_columns.Logical(), [](const ColumnDefinition &col) {
		return col.Category() == TableColumnType::GENERATED_STORED;
	});

	// Snapshot the parsed SET expressions before the bind loop consumes them.
	identifier_map_t<unique_ptr<ParsedExpression>> parsed_set_exprs;
	if (has_stored_generated) {
		parsed_set_exprs.reserve(set_info.columns.size());
		for (idx_t i = 0; i < set_info.columns.size(); i++) {
			parsed_set_exprs[set_info.columns[i]] = set_info.expressions[i]->Copy();
		}
	}

	for (idx_t i = 0; i < set_info.columns.size(); i++) {
		auto &colname = set_info.columns[i];
		auto &expr = set_info.expressions[i];
		if (!table.ColumnExists(colname)) {
			vector<string> column_names;
			for (auto &col : all_columns.Physical()) {
				column_names.emplace_back(col.Name().GetIdentifierName());
			}
			auto candidates =
			    StringUtil::CandidatesErrorMessage(column_names, colname.GetIdentifierName(), "Did you mean");
			throw BinderException("Referenced update column %s not found in table!\n%s", colname.GetIdentifierName(),
			                      candidates);
		}
		auto &column = table.GetColumn(colname);
		if (column.Generated()) {
			throw BinderException("Cant update column \"%s\" because it is a generated column!", column.Name());
		}
		if (absl::c_contains(columns, column.Physical())) {
			throw BinderException("Multiple assignments to same column \"%s\"", colname);
		}
		append_assignment(column, expr);
	}

	if (!has_stored_generated) {
		return;
	}
	// Recompute each stored generated column whose value depends on an assigned
	// column. Generated-column references are expanded inline -- the same
	// recursion the binder uses for INSERT defaults and SELECT -- and assigned
	// columns are substituted with their new value, so chained and virtual
	// dependencies resolve without an explicit ordering pass.
	for (auto &column : all_columns.Logical()) {
		if (column.Category() != TableColumnType::GENERATED_STORED) {
			continue;
		}
		auto recompute = column.GeneratedExpression().Copy();
		bool depends_on_update = false;
		ExpandStoredGeneratedExpression(recompute, table, [&](const Identifier &name) -> unique_ptr<ParsedExpression> {
			auto entry = parsed_set_exprs.find(name);
			if (entry == parsed_set_exprs.end()) {
				return nullptr;
			}
			// Assigned column: substitute its new value (DEFAULT means the column's default expression, or NULL
			// when it has none). Its own refs read the old row, so it is not expanded further.
			depends_on_update = true;
			if (entry->second->GetExpressionType() != ExpressionType::VALUE_DEFAULT) {
				return entry->second->Copy();
			}
			auto &base_col = table.GetColumn(name);
			return base_col.HasDefaultValue() ? base_col.DefaultValue().Copy()
			                                  : make_uniq<ConstantExpression>(Value(base_col.Type()));
		});
		if (depends_on_update) {
			append_assignment(column, recompute);
		}
	}
}

// This creates a LogicalProjection and moves 'root' into it as a child
// unless there are no expressions to project, in which case it just returns 'root'
unique_ptr<LogicalOperator> Binder::BindUpdateSet(LogicalOperator &op, unique_ptr<LogicalOperator> root,
                                                  UpdateSetInfo &set_info, TableCatalogEntry &table,
                                                  const vector<unique_ptr<Expression>> &bound_defaults,
                                                  vector<PhysicalIndex> &columns, bool prioritize_table_when_binding) {
	auto proj_index = GenerateTableIndex();

	vector<unique_ptr<Expression>> projection_expressions;
	BindUpdateSet(proj_index, root, set_info, table, columns, bound_defaults, op.expressions, projection_expressions,
	              prioritize_table_when_binding);
	if (op.type != LogicalOperatorType::LOGICAL_UPDATE && projection_expressions.empty()) {
		return root;
	}
	// now create the projection
	auto proj = make_uniq<LogicalProjection>(proj_index, std::move(projection_expressions));
	proj->AddChild(std::move(root));
	return unique_ptr_cast<LogicalProjection, LogicalOperator>(std::move(proj));
}

void Binder::BindRowIdColumns(TableCatalogEntry &table, LogicalGet &get, vector<unique_ptr<Expression>> &expressions) {
	auto row_id_columns = table.GetRowIdColumns();
	auto virtual_columns = table.GetVirtualColumns();
	auto &column_ids = get.GetColumnIds();
	for (auto &row_id_column : row_id_columns) {
		auto row_id_entry = virtual_columns.find(row_id_column);
		if (row_id_entry == virtual_columns.end()) {
			throw InternalException(
			    "BindRowIdColumns could not find the row id column in the virtual columns list of the table");
		}
		// check if this column has already been projected
		idx_t column_idx;
		for (column_idx = 0; column_idx < column_ids.size(); ++column_idx) {
			if (column_ids[column_idx].GetPrimaryIndex() == row_id_column) {
				// it has! avoid projecting it again
				break;
			}
		}
		auto row_id_expr = make_uniq<BoundColumnRefExpression>(
		    row_id_entry->second.type, ColumnBinding(get.table_index, ProjectionIndex(column_idx)));
		row_id_expr->SetAlias(row_id_entry->second.name);
		expressions.push_back(std::move(row_id_expr));
		if (column_idx == column_ids.size()) {
			get.AddColumnId(row_id_column);
		}
	}
}

BoundStatement Binder::Bind(UpdateStatement &stmt) {
	return Bind(*stmt.node);
}

BoundStatement Binder::BindNode(UpdateQueryNode &node) {
	unique_ptr<LogicalOperator> root;

	// visit the table reference (the UPDATE write target; SereneDB defers the
	// target scan's SELECT-privilege check to plan time -- see SdbBindWriteTarget)
	auto bound_table = SdbBindWriteTarget(*node.table);
	if (bound_table.plan->type != LogicalOperatorType::LOGICAL_GET) {
		throw BinderException("Can only update base table");
	}
	auto &bound_table_get = bound_table.plan->Cast<LogicalGet>();
	auto table_ptr = bound_table_get.GetTable();
	if (!table_ptr) {
		throw BinderException("Can only update base table");
	}
	if (node.table->type == TableReferenceType::BASE_TABLE) {
		// A catalog may delegate the scan of its table to a storage table in
		// another catalog; the update targets the entry the name resolves to.
		auto &target_ref = node.table->Cast<BaseTableRef>();
		EntryLookupInfo table_lookup(CatalogType::TABLE_ENTRY, target_ref.GetQualifiedName());
		auto resolved = Catalog::GetEntry(context, table_lookup, OnEntryNotFound::RETURN_NULL);
		if (resolved && resolved->type == CatalogType::TABLE_ENTRY) {
			table_ptr = &resolved->Cast<TableCatalogEntry>();
		}
	}
	auto &table = *table_ptr;

	if (auto expanded = TryExpandTriggers(node, table, TriggerEventType::UPDATE_EVENT)) {
		return std::move(*expanded);
	}

	optional_ptr<LogicalGet> get;
	if (node.from_table) {
		auto from_binder = Binder::CreateBinder(context, this);
		BoundJoinRef bound_crossproduct(JoinRefType::CROSS);
		bound_crossproduct.left = std::move(bound_table);
		bound_crossproduct.right = from_binder->Bind(*node.from_table);
		root = CreatePlan(bound_crossproduct);
		get = &root->children[0]->Cast<LogicalGet>();
		bind_context.AddContext(std::move(from_binder->bind_context));
	} else {
		root = std::move(bound_table.plan);
		get = &root->Cast<LogicalGet>();
	}

	if (!table.temporary) {
		// update of persistent table: not read only!
		auto &properties = GetStatementProperties();
		properties.RegisterDBModify(table.GetStorageCatalog(context), context, DatabaseModificationType::UPDATE_DATA);
	}
	auto update = make_uniq<LogicalUpdate>(table);

	// set return_chunk boolean early because it needs uses update_is_del_and_insert logic
	if (!node.returning_list.empty()) {
		update->return_chunk = true;
	}
	// bind the default values
	auto &catalog_name = table.ParentCatalog().GetName();
	auto &schema_name = table.ParentSchema().name;
	BindDefaultValues(table.GetColumns(), update->bound_defaults, catalog_name.GetIdentifierName(),
	                  schema_name.GetIdentifierName());
	update->bound_constraints = BindConstraints(table);

	// project any additional columns required for the condition/expressions
	if (node.set_info->condition) {
		WhereBinder binder(*this, context);
		auto condition = binder.Bind(node.set_info->condition);

		PlanSubqueries(condition, root);
		auto filter = make_uniq<LogicalFilter>(std::move(condition));
		filter->AddChild(std::move(root));
		root = std::move(filter);
	}

	D_ASSERT(node.set_info);
	D_ASSERT(node.set_info->columns.size() == node.set_info->expressions.size());

	auto proj_tmp = BindUpdateSet(*update, std::move(root), *node.set_info, table, update->bound_defaults,
	                              update->columns, node.prioritize_table_when_binding);
	D_ASSERT(proj_tmp->type == LogicalOperatorType::LOGICAL_PROJECTION);
	auto proj = unique_ptr_cast<LogicalOperator, LogicalProjection>(std::move(proj_tmp));

	// bind any extra columns necessary for CHECK constraints or indexes;
	// storage-derived decisions (index updates force delete+insert) come
	// from the scan-bound table when the catalog delegates storage
	auto storage_table = get->GetTable();
	if (storage_table && storage_table.get() != &table) {
		storage_table->BindUpdateConstraints(*this, *get, *proj, *update, context);
	} else {
		table.BindUpdateConstraints(*this, *get, *proj, *update, context);
	}

	// finally bind the row id column and add them to the projection list
	BindRowIdColumns(table, *get, proj->expressions);

	// set the projection as child of the update node and finalize the result
	update->AddChild(std::move(proj));

	auto update_table_index = GenerateTableIndex();
	update->table_index = update_table_index;
	if (!node.returning_list.empty()) {
		unique_ptr<LogicalOperator> update_as_logicaloperator = std::move(update);

		return BindReturning(std::move(node.returning_list), table, node.table->alias, update_table_index,
		                     std::move(update_as_logicaloperator));
	}

	BoundStatement result;
	result.names = {"Count"};
	result.types = {LogicalType::BIGINT};
	result.plan = std::move(update);

	auto &properties = GetStatementProperties();
	properties.output_type = QueryResultOutputType::FORCE_MATERIALIZED;
	properties.return_type = StatementReturnType::CHANGED_ROWS;
	return result;
}

} // namespace duckdb
