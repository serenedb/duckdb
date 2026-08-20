//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder/index_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/execution/index/unbound_index.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {

class BoundColumnRefExpression;

//! The IndexBinder binds indexes and expressions within index statements.
class IndexBinder : public ExpressionBinder {
public:
	IndexBinder(Binder &binder, ClientContext &context, optional_ptr<TableCatalogEntry> table = nullptr,
	            optional_ptr<CreateIndexInfo> info = nullptr);

	//! `bound_column_ids` is the projection the expressions bind against, filled by this call. A definition that
	//! came from a host catalog rather than from a scan carries no column list of its own, and this is it: the
	//! columns the keys just bound, in the order they bound them.
	//! `table_column_names` is the indexed table's logical column names as they are now. A stored definition names
	//! its keys as they were spelled when the index was built, and RENAME COLUMN does not rewrite it -- so the
	//! recorded position of each key is what says which column it is, and the current name at that position is what
	//! the expressions are re-spelled with before binding. Empty leaves the definition alone.
	unique_ptr<BoundIndex> BindIndex(const UnboundIndex &index,
	                                 optional_ptr<const vector<ColumnIndex>> bound_column_ids,
	                                 const vector<Identifier> &table_column_names);
	unique_ptr<LogicalOperator> BindCreateIndex(ClientContext &context, unique_ptr<CreateIndexInfo> create_index_info,
	                                            TableCatalogEntry &table_entry, unique_ptr<LogicalOperator> plan,
	                                            unique_ptr<AlterTableInfo> alter_table_info);

	static void InitCreateIndexInfo(LogicalGet &get, CreateIndexInfo &info, const Identifier &schema);

protected:
	BindResult BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
	                          bool root_expression = false) override;
	string UnsupportedAggregateMessage() override;

private:
	// Only for WAL replay.
	optional_ptr<TableCatalogEntry> table;
	optional_ptr<CreateIndexInfo> info;
};

} // namespace duckdb
