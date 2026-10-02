//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_security_barrier.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class LogicalSecurityBarrier : public LogicalOperator {
public:
	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_SECURITY_BARRIER;

public:
	explicit LogicalSecurityBarrier(unique_ptr<LogicalOperator> child);

public:
	vector<ColumnBinding> GetColumnBindings() override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);

	static bool CanCross(const Expression &expr);

protected:
	void ResolveTypes() override;

private:
	LogicalSecurityBarrier();
};

} // namespace duckdb
