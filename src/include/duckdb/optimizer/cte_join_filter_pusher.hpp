//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/cte_join_filter_pusher.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/insertion_order_preserving_map.hpp"
#include "duckdb/planner/column_binding.hpp"

namespace duckdb {

class LogicalOperator;
class LogicalCTERef;
class Optimizer;

class CTEJoinFilterPusher {
public:
	explicit CTEJoinFilterPusher(Optimizer &optimizer);
	void Optimize(LogicalOperator &op);

private:
	struct MaterializedCTEInfo {
		explicit MaterializedCTEInfo(LogicalOperator &cte) : materialized_cte(cte) {
		}
		LogicalOperator &materialized_cte;
		vector<reference<LogicalCTERef>> references;
		idx_t position = 0;
		idx_t body_begin = 0;
		idx_t body_end = 0;
	};

	void FindCandidates(LogicalOperator &op);
	bool HasValidDependency(const MaterializedCTEInfo &info);

private:
	Optimizer &optimizer;
	InsertionOrderPreservingMap<unique_ptr<MaterializedCTEInfo>> cte_info_map;
	idx_t cte_count = 0;
};

} // namespace duckdb
