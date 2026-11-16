//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/schema/physical_alter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"

namespace duckdb {

//! PhysicalAlter represents an ALTER TABLE command
class PhysicalAlter : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::ALTER;

public:
	explicit PhysicalAlter(PhysicalPlan &physical_plan, vector<unique_ptr<AlterInfo>> infos,
	                       idx_t estimated_cardinality)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::ALTER, {LogicalType::BOOLEAN}, estimated_cardinality),
	      infos(std::move(infos)) {
	}

	vector<unique_ptr<AlterInfo>> infos;

public:
	// Source interface
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}
};

} // namespace duckdb
