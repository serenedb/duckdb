#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class ClientContext;
class DataChunk;
struct WALDeleteRowRanges;

class TableLogStorage {
public:
	virtual ~TableLogStorage() = default;

	virtual void ReplayInsert(ClientContext &context, DataChunk &chunk) = 0;
	virtual void ReplayTruncate(ClientContext &context) = 0;
	virtual void ReplayAdoptSegments(ClientContext &context, vector<string> segments) = 0;
	virtual void ReplayDeleteRanges(ClientContext &context, const WALDeleteRowRanges &entry) = 0;
	virtual void ReplayRowPositionMaps(ClientContext &context, string maps) = 0;
};

} // namespace duckdb
