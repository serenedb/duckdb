//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/persistent/physical_insert.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/common/index_vector.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/storage/optimistic_data_writer.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"

namespace duckdb {
class SchemaCatalogEntry;
class TableCatalogEntry;

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class InsertGlobalState : public GlobalSinkState {
public:
	explicit InsertGlobalState(ClientContext &context, const vector<LogicalType> &return_types, DuckTableEntry &table);

public:
	mutex lock;
	DuckTableEntry &table;
	idx_t insert_count;
	ColumnDataCollection return_collection;
	//! Leftover thread-local collections (smaller than a row group) that are compacted and merged in Finalize.
	vector<PhysicalIndex> unmerged_collections;
};

class InsertLocalState : public LocalSinkState {
public:
	explicit InsertLocalState(const vector<unique_ptr<BoundConstraint>> &bound_constraints);

public:
	ConstraintState &GetConstraintState(DataTable &table, TableCatalogEntry &table_ref);

public:
	TableAppendState local_append_state;
	//! An index to the optimistic row group collection vector of the local table storage for this transaction.
	PhysicalIndex collection_index;
	unique_ptr<OptimisticDataWriter> optimistic_writer;
	unique_ptr<ConstraintState> constraint_state;
	const vector<unique_ptr<BoundConstraint>> &bound_constraints;
};

//! Physically insert a set of data into a table
class PhysicalInsert : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::INSERT;

public:
	//! INSERT INTO
	PhysicalInsert(PhysicalPlan &physical_plan, vector<LogicalType> types, DuckTableEntry &table,
	               vector<unique_ptr<BoundConstraint>> bound_constraints, idx_t estimated_cardinality,
	               bool return_chunk, bool parallel);
	//! CREATE TABLE AS
	PhysicalInsert(PhysicalPlan &physical_plan, LogicalOperator &op, SchemaCatalogEntry &schema,
	               unique_ptr<BoundCreateTableInfo> info, idx_t estimated_cardinality, bool parallel);

	//! The table to insert into
	optional_ptr<DuckTableEntry> insert_table;
	//! The insert types
	vector<LogicalType> insert_types;
	//! The bound constraints for the table
	vector<unique_ptr<BoundConstraint>> bound_constraints;
	//! If the returning statement is present, return the whole chunk
	bool return_chunk;
	//! Table schema, in case of CREATE TABLE AS
	optional_ptr<SchemaCatalogEntry> schema;
	//! Create table info, in case of CREATE TABLE AS
	unique_ptr<BoundCreateTableInfo> info;
	//! Whether or not the INSERT can be executed in parallel
	//! This insert is not order preserving if executed in parallel
	bool parallel;

public:
	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	ProgressData GetProgress(ClientContext &context, GlobalSourceState &gstate) const override;

	bool IsSource() const override {
		return true;
	}

public:
	// Sink interface
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;

	bool IsSink() const override {
		return true;
	}

	bool ParallelSink() const override {
		return parallel;
	}

	bool SinkOrderDependent() const override {
		return true;
	}

public:
	static void GetInsertInfo(const BoundCreateTableInfo &info, vector<LogicalType> &insert_types);
};

} // namespace duckdb
