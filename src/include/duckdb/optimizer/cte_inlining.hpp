//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/cte_inlining.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/insertion_order_preserving_map.hpp"
#include "duckdb/planner/bound_parameter_map.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/list.hpp"

#include <absl/container/flat_hash_map.h>

namespace duckdb {

class LogicalOperator;
class Optimizer;
struct BoundParameterData;

class CTEInlining {
public:
	explicit CTEInlining(Optimizer &optimizer);
	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);
	static bool EndsInAggregateOrDistinct(const LogicalOperator &op);

private:
	struct CTEScan {
		optional_ptr<LogicalOperator> op;
		reference<unique_ptr<LogicalOperator>> slot;
	};
	struct SubtreeInfo {
		bool has_side_effects = false;
		bool contains_delim_get = false;
		bool prevents_inlining = false;
		idx_t base_table_references = 0;

		void Add(const SubtreeInfo &other, idx_t times = 1);
	};

	SubtreeInfo TryInlining(unique_ptr<LogicalOperator> &op, bool collect_volatility);
	SubtreeInfo TryInliningCTE(unique_ptr<LogicalOperator> &op, const SubtreeInfo &definition, const SubtreeInfo &body,
	                           bool knows_volatility);
	idx_t Inline(LogicalOperator &materialized_cte, const vector<idx_t> &scan_ids, bool requires_copy);
	void RemoveCTE(unique_ptr<LogicalOperator> &op, bool definition_moved);
	bool RegisterScans(unique_ptr<LogicalOperator> &op);
	void UnregisterScans(LogicalOperator &op);
	void MoveScan(unique_ptr<LogicalOperator> &slot);

private:
	//! The optimizer
	Optimizer &optimizer;

	optional_ptr<bound_parameter_map_t> parameter_data;
	vector<CTEScan> scans;
	absl::flat_hash_map<const LogicalOperator *, idx_t> scan_index;
	absl::flat_hash_map<TableIndex, vector<idx_t>> cte_scans;
};

class PreventInlining : public LogicalOperatorVisitor {
public:
	PreventInlining() : prevent_inlining(false) {};

	void VisitOperator(LogicalOperator &op) override;
	bool prevent_inlining;

	void VisitExpression(unique_ptr<Expression> *expression) override;
};

} // namespace duckdb
