//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_lookup_join.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_unconditional_join.hpp"

namespace duckdb {

class LogicalLookupJoin : public LogicalUnconditionalJoin {
	LogicalLookupJoin() : LogicalUnconditionalJoin(LogicalOperatorType::LOGICAL_LOOKUP_JOIN) {};

public:
	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_LOOKUP_JOIN;

public:
	LogicalLookupJoin(unique_ptr<LogicalOperator> ids, unique_ptr<LogicalGet> source,
	                  vector<unique_ptr<Expression>> row_ids);

public:
	LogicalGet &GetSource() {
		return children[1]->Cast<LogicalGet>();
	}

	idx_t EstimateCardinality(ClientContext &context) override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
