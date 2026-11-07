#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_order.hpp"

namespace duckdb {

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalLimit &limit,
                                                                     unique_ptr<LogicalOperator> &node_ptr) {
	auto &child = *limit.children[0];
	if (child.type == LogicalOperatorType::LOGICAL_ORDER_BY &&
	    LogicalGet::TopNConsumer(*child.children[0], child.Cast<LogicalOrder>().orders.size(), limit.limit_val,
	                             limit.offset_val)) {
		uncompressed = &child;
	}
	// propagate statistics in the child node
	auto child_stats = PropagateStatistics(limit.children[0]);
	// return the node stats, with as expected cardinality the amount specified in the limit
	if (limit.limit_val.Type() == LimitNodeType::CONSTANT_VALUE) {
		auto constant_limit = limit.limit_val.GetConstantValue();
		if (limit.offset_val.Type() == LimitNodeType::UNSET && child_stats && child_stats->has_max_cardinality &&
		    child_stats->max_cardinality < constant_limit) {
			node_ptr = std::move(limit.children[0]);
			return child_stats;
		}
		return make_uniq<NodeStatistics>(constant_limit, constant_limit);
	}
	return nullptr;
}

} // namespace duckdb
