#include "duckdb/planner/expression_binder/index_binder.hpp"

#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/execution/index/unbound_index.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/planner/operator/logical_create_index.hpp"

namespace duckdb {

IndexBinder::IndexBinder(Binder &binder, ClientContext &context, optional_ptr<TableCatalogEntry> table,
                         optional_ptr<CreateIndexInfo> info)
    : ExpressionBinder(binder, context), table(table), info(info) {
}

//! Old spelling -> current spelling for every key column of `info` whose name moved. The key's position in the
//! table is the identity that a rename leaves alone: an ALTER that would shift it is refused while the index
//! exists, so `column_ids` still points at the same column and the name there is what the key is called now.
static case_insensitive_map_t<Identifier> RenamedKeyColumns(const CreateIndexInfo &info,
                                                            const vector<Identifier> &table_column_names) {
	case_insensitive_map_t<Identifier> renames;
	if (info.names.empty() || table_column_names.empty()) {
		return renames;
	}
	for (auto column_id : info.column_ids) {
		if (column_id >= info.names.size() || column_id >= table_column_names.size()) {
			continue;
		}
		auto &was = info.names[column_id];
		auto &is = table_column_names[column_id];
		if (was.GetIdentifierName() != is.GetIdentifierName()) {
			renames.emplace(was.GetIdentifierName(), is);
		}
	}
	return renames;
}

static void ApplyRenames(ParsedExpression &expr, const case_insensitive_map_t<Identifier> &renames) {
	ParsedExpressionIterator::VisitExpressionMutable<ColumnRefExpression>(expr, [&](ColumnRefExpression &colref) {
		auto entry = renames.find(colref.ColumnNames().back().GetIdentifierName());
		if (entry != renames.end()) {
			colref.ColumnNamesMutable().back() = entry->second;
		}
	});
}

unique_ptr<BoundIndex> IndexBinder::BindIndex(const UnboundIndex &unbound_index,
                                              const vector<ColumnIndex> &bound_column_ids,
                                              const vector<Identifier> &table_column_names) {
	auto &index_type_name = unbound_index.GetIndexType();
	// Do we know the type of this index now?
	auto index_type = context.db->config.GetIndexTypes().FindByName(index_type_name);
	if (!index_type) {
		throw MissingExtensionException("Cannot bind index '%s', unknown index type '%s'. You need to load the "
		                                "extension that provides this index type before table '%s' can be modified.",
		                                unbound_index.GetTableName(), index_type_name, unbound_index.GetTableName());
	}

	auto &create_info = unbound_index.GetCreateInfo();
	auto &storage_info = unbound_index.GetStorageInfo();
	auto &parsed_expressions = unbound_index.GetParsedExpressions();

	// bind the parsed expressions to create unbound expressions
	auto renames = RenamedKeyColumns(create_info, table_column_names);
	vector<unique_ptr<Expression>> unbound_expressions;
	unbound_expressions.reserve(parsed_expressions.size());
	for (auto &expr : parsed_expressions) {
		auto copy = expr->Copy();
		if (!renames.empty()) {
			ApplyRenames(*copy, renames);
		}
		unbound_expressions.push_back(Bind(copy));
	}

	auto column_ids = create_info.column_ids;
	if (column_ids.empty()) {
		column_ids.reserve(bound_column_ids.size());
		for (auto &column_id : bound_column_ids) {
			column_ids.push_back(column_id.GetPrimaryIndex());
		}
	}

	CreateIndexInput input(context, unbound_index.table_io_manager, unbound_index.db, create_info.constraint_type,
	                       create_info.GetIndexName(), column_ids, unbound_expressions, storage_info,
	                       create_info.options);

	return index_type->create_instance(input);
}

void IndexBinder::InitCreateIndexInfo(LogicalGet &get, CreateIndexInfo &info, const Identifier &schema) {
	auto &column_ids = get.GetColumnIds();
	for (auto &column_id : column_ids) {
		if (column_id.IsRowIdColumn()) {
			throw BinderException("cannot create an index on the rowid");
		}
		auto col_id = column_id.GetPrimaryIndex();
		info.column_ids.push_back(col_id);
		info.scan_types.push_back(get.returned_types[col_id]);
	}

	info.scan_types.emplace_back(LogicalType::ROW_TYPE);
	info.names = get.names;
	info.SetQualifiedName(
	    QualifiedName(get.GetTable()->catalog.GetName(), Identifier(schema), info.GetQualifiedName().Name()));
	get.AddColumnId(COLUMN_IDENTIFIER_ROW_ID);
}

unique_ptr<LogicalOperator> IndexBinder::BindCreateIndex(ClientContext &context,
                                                         unique_ptr<CreateIndexInfo> create_index_info,
                                                         TableCatalogEntry &table_entry,
                                                         unique_ptr<LogicalOperator> plan,
                                                         unique_ptr<AlterTableInfo> alter_table_info) {
	// Add the dependencies.
	auto &dependencies = create_index_info->dependencies;
	auto &catalog = Catalog::GetCatalog(context, create_index_info->GetQualifiedName().Catalog());
	SetCatalogLookupCallback([&dependencies, &catalog](CatalogEntry &entry) {
		if (&catalog != &entry.ParentCatalog()) {
			return;
		}
		dependencies.AddDependency(entry);
	});

	// Bind the index expressions.
	vector<unique_ptr<Expression>> expressions;
	for (auto &expr : create_index_info->expressions) {
		expressions.push_back(Bind(expr));
	}

	auto &get = plan->Cast<LogicalGet>();
	InitCreateIndexInfo(get, *create_index_info, table_entry.ParentSchema().name);
	auto &bind_data = get.bind_data->Cast<TableScanBindData>();
	bind_data.is_create_index = true;

	auto result = make_uniq<LogicalCreateIndex>(std::move(create_index_info), std::move(expressions), table_entry,
	                                            std::move(alter_table_info));
	result->children.push_back(std::move(plan));
	return std::move(result);
}

BindResult IndexBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	auto &expr = *expr_ptr;
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::WINDOW:
		return BindResult(BinderException::Unsupported(expr, "window functions are not allowed in index expressions"));
	case ExpressionClass::SUBQUERY:
		return BindResult(BinderException::Unsupported(expr, "cannot use subquery in index expressions"));
	default:
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	}
}

string IndexBinder::UnsupportedAggregateMessage() {
	return "aggregate functions are not allowed in index expressions";
}

} // namespace duckdb
