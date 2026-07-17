#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/statement/vacuum_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_vacuum.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

namespace duckdb {

void Binder::BindVacuumTable(LogicalVacuum &vacuum, unique_ptr<LogicalOperator> &root) {
	auto &info = vacuum.GetInfo();
	if (!info.has_table) {
		return;
	}

	D_ASSERT(vacuum.column_id_map.empty());

	auto bound_table = Bind(*info.ref);
	if (bound_table.plan->type != LogicalOperatorType::LOGICAL_GET) {
		throw BinderException("Can only vacuum or analyze base tables");
	}
	auto table_scan = std::move(bound_table.plan);
	auto &get = table_scan->Cast<LogicalGet>();
	auto table_ptr = get.GetTable();
	if (!table_ptr) {
		throw BinderException("Can only vacuum or analyze base tables");
	}
	auto &table = *table_ptr;
	vacuum.SetTable(table);

	// Bind columns against the name the table ref was bound under, not the
	// catalog entry's name: for facade tables the resolved entry is the hidden
	// store table (a different, qualified name), so table.name would not match
	// the binding alias.
	auto &base_ref = info.ref->Cast<BaseTableRef>();
	auto &binding_name = info.ref->alias.empty() ? base_ref.Table() : info.ref->alias;
	ErrorData binding_error;
	auto binding = bind_context.GetBinding(binding_name, binding_error);
	if (!binding) {
		binding_error.Throw();
	}
	vector<unique_ptr<Expression>> select_list;
	auto &columns = info.columns;
	// A generated column of this entry has no statistics to gather: ColumnList
	// leaves every one of them out of the physical columns, STORED included, so
	// LogicalToPhysical below would throw for it and the column bind rejects a
	// virtual one outright. A facade table's names belong to no column of the
	// entry the ref resolved to -- that is the hidden store table underneath,
	// where every facade column, however the facade reports it, is a stored
	// column of its own.
	auto analyzable = [&table](const Identifier &name) {
		return !table.ColumnExists(name) || !table.GetColumn(name).Generated();
	};
	if (columns.empty()) {
		// Empty means ALL columns should be vacuumed/analyzed. Take the names from
		// the binding rather than the entry, for the same reason the columns bind
		// against the alias: a facade table resolves to the store table, whose
		// columns carry id-derived names the binding does not expose.
		for (auto &name : binding->GetColumnNames()) {
			if (analyzable(name)) {
				columns.emplace_back(name);
			}
		}
	}

	identifier_set_t column_name_set;
	vector<Identifier> analyzed_column_names;
	for (auto &col_name : columns) {
		if (column_name_set.count(col_name) > 0) {
			throw BinderException("cannot vacuum or analyze the same column twice, i.e., there is a duplicate entry in "
			                      "the list of column names");
		}
		column_name_set.insert(col_name);
		if (!analyzable(col_name)) {
			throw BinderException("cannot vacuum or analyze generated column \"%s\" - it has no storage of its own; "
			                      "specify columns that are stored",
			                      col_name);
		}
		analyzed_column_names.emplace_back(col_name);
		ColumnRefExpression colref(col_name, binding_name);
		auto result = bind_context.BindColumn(colref, 0);
		if (result.HasError()) {
			result.error.Throw();
		}
		select_list.push_back(std::move(result.expression));
	}
	info.columns = analyzed_column_names;

	auto &column_ids = get.GetColumnIds();
	D_ASSERT(select_list.size() == column_ids.size());
	D_ASSERT(info.columns.size() == column_ids.size());
	for (idx_t i = 0; i < column_ids.size(); i++) {
		vacuum.column_id_map[i] = table.GetColumns().LogicalToPhysical(column_ids[i].ToLogical()).index;
	}

	auto projection = make_uniq<LogicalProjection>(GenerateTableIndex(), std::move(select_list));
	projection->children.push_back(std::move(table_scan));

	root = std::move(projection);
}

BoundStatement Binder::Bind(VacuumStatement &stmt) {
	BoundStatement result;

	unique_ptr<LogicalOperator> root;

	auto vacuum = make_uniq<LogicalVacuum>(std::move(stmt.info));
	BindVacuumTable(*vacuum, root);
	if (root) {
		vacuum->children.push_back(std::move(root));
	}

	result.names = {"Success"};
	result.types = {LogicalType::BOOLEAN};
	result.plan = std::move(vacuum);

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::NOTHING;
	return result;
}

} // namespace duckdb
