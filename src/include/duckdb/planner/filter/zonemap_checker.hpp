//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/filter/zonemap_checker.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/filter_propagate_result.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {

class BaseStatistics;
class ClientContext;
class Expression;

//! A filter expression's statistics check compiled once per filter, so zonemap probes (row
//! groups, parquet row groups and pages, codec-internal groups) stop re-walking the expression
//! tree and re-building Values on every call - the pruning counterpart of the fast filter
//! executors. Nodes exist only for shapes whose CheckExpressionStatistics semantics they
//! reproduce exactly; everything else compiles to a fallback node that delegates its subtree to
//! the generic walk, so a compiled check is never less capable than the walk it replaces.
class ZonemapChecker {
public:
	virtual ~ZonemapChecker() = default;

	//! Compile the statistics check of a filter expression; never fails. The expression must
	//! outlive the checker (fallback nodes reference their subtrees).
	static unique_ptr<ZonemapChecker> Compile(const Expression &expr);

	//! Verdict against the given statistics. `context` is only consulted by fallback nodes
	//! (context-dependent function statistics), like CheckExpressionStatistics.
	virtual FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const = 0;

	//! True when no node in this subtree delegates to the expression walk. Hot paths that probe
	//! at fine granularity (per codec group) gate on this to stay cheap.
	virtual bool IsFullyCompiled() const {
		return true;
	}
};

} // namespace duckdb
