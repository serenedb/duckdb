//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/lookup_tasks.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/data_chunk.hpp"

#include <functional>

namespace duckdb {

//! The rows one task of a lookup found, appended to the lookup's output once every task finished
struct LookupTaskOutput {
	DataChunk found;
	vector<idx_t> found_rows;

	//! Empties `found`, making room for `count` rows of `types`
	void ResetFound(ClientContext &context, const vector<LogicalType> &types, idx_t count);
	//! Appends the found rows to the output columns whose `column_map` entry is valid, and their row indexes to
	//! `survivors`, from row `w` on; returns the row after them
	idx_t AppendFound(DataChunk &output, const idx_t *column_map, idx_t *survivors, idx_t w);
};

//! Calls `step(task, index)` for every index in [0, count) on `tasks` tasks, each taking the next index
void RunLookupTasks(ClientContext &context, idx_t tasks, idx_t count, const std::function<void(idx_t, idx_t)> &step);

} // namespace duckdb
