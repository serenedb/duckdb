//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/join/physical_lookup_join.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

class PhysicalLookupJoin : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::LOOKUP_JOIN;

public:
	PhysicalLookupJoin(PhysicalPlan &physical_plan, vector<LogicalType> types, PhysicalOperator &ids,
	                   PhysicalTableScan &source, TableFunction lookup, vector<idx_t> row_id_columns,
	                   vector<string> key_names, idx_t estimated_cardinality);

	TableFunction lookup;
	vector<idx_t> row_id_columns;
	vector<string> key_names;
	vector<idx_t> output_columns;

public:
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}
	OrderPreservationType SourceOrder() const override {
		return OrderPreservationType::FIXED_ORDER;
	}

public:
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;

	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}
	bool SinkOrderDependent() const override {
		return true;
	}

	void FetchBatches(ExecutionContext &context, GlobalSinkState &sink) const;

	InsertionOrderPreservingMap<string> ParamsToString() const override;

	void BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) override;
	vector<const_reference<PhysicalOperator>> GetSources() const override;

private:
	const PhysicalTableScan &Source() const;
};

} // namespace duckdb
