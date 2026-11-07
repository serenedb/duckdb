//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/late_materialization.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/optimizer/remove_unused_columns.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/common/table_column.hpp"

namespace duckdb {
class LogicalOperator;
class LogicalGet;
class LogicalLimit;
class Optimizer;

//! Transform
class LateMaterialization : public BaseColumnPruner {
public:
	explicit LateMaterialization(Optimizer &optimizer);

	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);

private:
	bool TryLateMaterialization(unique_ptr<LogicalOperator> &op, idx_t row_count, bool semi_join,
	                            bool unfiltered_lookup);
	bool SupportsLookup(LogicalGet &get, TableFunctionLookupInfo &info);
	bool TryLookupJoin(unique_ptr<LogicalOperator> &op, LogicalGet &get,
	                   const vector<reference<LogicalOperator>> &source_operators,
	                   const column_binding_set_t &required_columns, const TableFunctionLookupInfo &info);
	void LoadRowIdColumns(LogicalGet &get);
	vector<unique_ptr<Expression>> ConstructFinalProjection(LogicalGet &lhs_get, idx_t lhs_columns,
	                                                        const vector<reference<LogicalOperator>> &source_operators);

	unique_ptr<LogicalGet> ConstructLHS(LogicalGet &get);
	vector<ColumnBinding> ConstructRHS(unique_ptr<LogicalOperator> &op, const vector<ProjectionIndex> &indexes);
	vector<idx_t> GetOrInsertRowIds(LogicalGet &get);

	void ReplaceTopLevelTableIndex(LogicalOperator &op, TableIndex new_index);
	void ReplaceTableReferences(unique_ptr<Expression> &expr, TableIndex new_table_index);
	unique_ptr<Expression> GetExpression(LogicalOperator &op, ProjectionIndex column_index);
	void ReplaceExpressionReferences(LogicalOperator &next_op, unique_ptr<Expression> &expr);
	bool OptimizeLargeLimit(LogicalLimit &limit, idx_t limit_val, bool has_offset);

private:
	Optimizer &optimizer;
	//! The max row count for which we will consider late materialization
	idx_t max_row_count;
	//! The max row count for which we will consider a lookup join
	idx_t lookup_max_row_count;
	//! The row-id column ids
	vector<column_t> row_id_column_ids;
	//! The row-id columns
	vector<TableColumn> row_id_columns;
	bool lookups = true;
};

} // namespace duckdb
