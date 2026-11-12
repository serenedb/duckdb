//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/row_id_deduplicator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace duckdb {

class ClientContext;
class GroupedAggregateHashTable;

//! Registers row IDs across chunks. The caller owns locking and decides whether duplicates are kept or rejected.
class RowIdDeduplicator {
public:
	RowIdDeduplicator(ClientContext &context, vector<LogicalType> row_id_types);
	~RowIdDeduplicator();

	//! Registers the trailing row-ID columns in input, starting at row_id_start.
	idx_t Register(DataChunk &input, idx_t row_id_start, optional_ptr<SelectionVector> sel = nullptr);
	//! Registers the first count entries of a single-column row-ID vector.
	idx_t Register(const Vector &row_ids, idx_t count, optional_ptr<SelectionVector> sel = nullptr);

private:
	idx_t Register(DataChunk &row_ids, optional_ptr<SelectionVector> sel);

private:
	vector<LogicalType> row_id_types;
	unique_ptr<GroupedAggregateHashTable> hash_table;
	Vector addresses;
	SelectionVector new_groups;
	DataChunk row_id_chunk;
};

//! The row ids a statement's threads register; a table whose row ids allow it provides a cheaper one.
class RowIdSet {
public:
	virtual ~RowIdSet() = default;

	//! Registers the trailing row-ID columns of input, starting at row_id_start, and returns how many were new. May
	//! slice input to the new rows.
	virtual idx_t Register(DataChunk &input, idx_t row_id_start) = 0;
};

//! A RowIdDeduplicator under a lock: works for any row-ID layout.
class LockedRowIdSet final : public RowIdSet {
public:
	LockedRowIdSet(ClientContext &context, vector<LogicalType> row_id_types);

	idx_t Register(DataChunk &input, idx_t row_id_start) final;

private:
	mutex lock;
	RowIdDeduplicator rows;
};

} // namespace duckdb
