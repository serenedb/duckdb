//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_alter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"

namespace duckdb {

//! LogicalAlter represents an ALTER statement
class LogicalAlter : public LogicalOperator {
public:
	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_ALTER;

public:
	explicit LogicalAlter(vector<unique_ptr<AlterInfo>> infos);
	~LogicalAlter() override;

	vector<unique_ptr<AlterInfo>> infos;

public:
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);
	idx_t EstimateCardinality(ClientContext &context) override;
	bool RequireOptimizer() const override {
		return false;
	}

protected:
	void ResolveTypes() override;
};

} // namespace duckdb
