//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_filter_pushdown_optimizer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/reference_map.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/column_binding_map.hpp"

namespace duckdb {
class Optimizer;
struct JoinFilterPushdownColumn;
struct PushdownFilterTarget;

enum class PushdownTargetUse : uint8_t { JOIN_FILTER, BOUND, SCAN_ORDER_BOUND };

//! The JoinFilterPushdownOptimizer links comparison joins to data sources to enable dynamic execution-time filter
//! pushdown
class JoinFilterPushdownOptimizer : public LogicalOperatorVisitor {
public:
	explicit JoinFilterPushdownOptimizer(Optimizer &optimizer);

public:
	void VisitOperator(LogicalOperator &op) override;
	static void GetPushdownFilterTargets(LogicalOperator &op, vector<JoinFilterPushdownColumn> columns,
	                                     vector<PushdownFilterTarget> &targets,
	                                     PushdownTargetUse use = PushdownTargetUse::JOIN_FILTER);

	static bool IsFiltering(const LogicalOperator &op, reference_map_t<const LogicalOperator, bool> &filtering);

private:
	void GenerateJoinFilters(LogicalComparisonJoin &join);

private:
	Optimizer &optimizer;
	reference_map_t<const LogicalOperator, bool> filtering;
};
} // namespace duckdb
