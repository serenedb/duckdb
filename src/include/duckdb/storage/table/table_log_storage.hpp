#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_idx.hpp"

namespace duckdb {

class ClientContext;
class DataChunk;

class TableLogStorage {
public:
	virtual ~TableLogStorage() = default;

	virtual void ReplayInsert(ClientContext &context, idx_t tick, DataChunk &chunk, optional_idx row_start) = 0;
	virtual void ReplayDelete(ClientContext &context, idx_t tick, DataChunk &chunk) = 0;
	virtual void ReplayTruncate(ClientContext &context, idx_t tick) = 0;
	virtual void ReplayAdoptSegments(ClientContext &context, idx_t tick, vector<string> segments) = 0;
	virtual void FinishReplay() = 0;
	virtual void Checkpoint() = 0;
};

} // namespace duckdb
