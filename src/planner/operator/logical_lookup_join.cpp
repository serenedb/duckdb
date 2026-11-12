#include "duckdb/planner/operator/logical_lookup_join.hpp"

namespace duckdb {

LogicalLookupJoin::LogicalLookupJoin(unique_ptr<LogicalOperator> ids, unique_ptr<LogicalGet> source,
                                     vector<unique_ptr<Expression>> row_ids)
    : LogicalUnconditionalJoin(LogicalOperatorType::LOGICAL_LOOKUP_JOIN, std::move(ids), std::move(source)) {
	expressions = std::move(row_ids);
	if (children[0]->has_estimated_cardinality) {
		SetEstimatedCardinality(children[0]->estimated_cardinality);
	}
}

idx_t LogicalLookupJoin::EstimateCardinality(ClientContext &context) {
	if (!has_estimated_cardinality) {
		SetEstimatedCardinality(children[0]->EstimateCardinality(context));
	}
	return estimated_cardinality;
}

} // namespace duckdb
