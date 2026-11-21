//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/schema/physical_drop.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"

namespace duckdb {

//! PhysicalDrop represents a DROP [...] command
class PhysicalDrop : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::DROP;

public:
	explicit PhysicalDrop(PhysicalPlan &physical_plan, vector<unique_ptr<DropInfo>> infos, idx_t estimated_cardinality)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::DROP, {LogicalType::BOOLEAN}, estimated_cardinality),
	      infos(std::move(infos)) {
	}

	vector<unique_ptr<DropInfo>> infos;

public:
	// Source interface
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}
};

} // namespace duckdb
