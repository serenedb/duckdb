#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

class ClientContext;
class DataChunk;

class TableLogStorage {
public:
	virtual ~TableLogStorage() = default;

	virtual void ReplayInsert(ClientContext &context, DataChunk &chunk, optional_idx row_start) = 0;
	virtual void ReplayDelete(ClientContext &context, DataChunk &chunk) = 0;
	virtual void ReplayTruncate(ClientContext &context) = 0;
	virtual void ReplayAdoptSegments(ClientContext &context, vector<string> segments) = 0;
	virtual void ReplayExpire(ClientContext &context, idx_t column, const Value &cutoff) = 0;
};

} // namespace duckdb
