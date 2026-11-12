//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/cte_filter_pusher.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/insertion_order_preserving_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/table_index.hpp"

#include <absl/container/btree_map.h>

namespace duckdb {

class LogicalOperator;
class LogicalCTERef;
class Optimizer;

class CTEFilterPusher {
public:
	explicit CTEFilterPusher(Optimizer &optimizer);
	//! Push consumer predicates and eligible join-key restrictions into materialized CTEs.
	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);
	//! Discard planner guarantees before transformations that do not preserve consumer identity.
	static void ClearDependencies(LogicalOperator &op);

private:
	struct CTEReference {
		optional_ptr<LogicalOperator> filter;
		reference<LogicalCTERef> ref;
	};
	//! CTE info needed for creating OR filters that can be pushed down
	struct MaterializedCTEInfo {
		explicit MaterializedCTEInfo(LogicalOperator &materialized_cte);
		LogicalOperator &materialized_cte;
		absl::btree_multimap<idx_t, CTEReference> references;
		idx_t definition_begin = 0;
		idx_t definition_end = 0;
		bool has_filter_dependency;

		vector<reference<LogicalOperator>> Filters();
		vector<reference<LogicalCTERef>> References() const;
		bool AllReferencesAreFiltered() const;
	};

private:
	//! Find all materialized CTEs and their refs
	void FindCandidates(LogicalOperator &op);
	void RefreshDefinition(MaterializedCTEInfo &info, const unordered_set<TableIndex> &referenced_ctes);
	bool CanPushFilter(MaterializedCTEInfo &info);
	bool HasValidDependency(const MaterializedCTEInfo &info);
	//! Creates an OR filter and pushes it into a materialized CTE
	void PushFilterIntoCTE(MaterializedCTEInfo &info);

private:
	//! The optimizer
	Optimizer &optimizer;
	//! Mapping from CTE index to CTE info, order preserving so deepest CTEs are done first
	InsertionOrderPreservingMap<unique_ptr<MaterializedCTEInfo>> cte_info_map;
	idx_t position = 0;
	bool refreshing = false;
};

} // namespace duckdb
