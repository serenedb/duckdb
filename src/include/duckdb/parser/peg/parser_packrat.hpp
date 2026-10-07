//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parser_packrat.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/storage/arena_allocator.hpp"

namespace duckdb {
class Matcher;
class ParseResult;

struct ParserPackratEntry {
	bool success = false;
	idx_t token_index_after = 0;
	idx_t max_token_index_seen = 0;
	optional_ptr<ParseResult> result;
};

class ParserPackratCache {
public:
	ParserPackratCache(ArenaAllocator &arena, idx_t first_token, idx_t slot_count);

	optional_ptr<const ParserPackratEntry> Lookup(idx_t slot, idx_t token_index) const {
		auto offset = token_index - first_token;
		auto row = offset / BLOCK_TOKENS;
		if (slot >= slot_count || row >= row_count) {
			return nullptr;
		}
		auto block = blocks[row * slot_count + slot];
		auto index = offset % BLOCK_TOKENS;
		if (!block || !CachedFlags(block)[index]) {
			return nullptr;
		}
		return block[index];
	}
	void Store(idx_t slot, idx_t token_index, const ParserPackratEntry &entry);

private:
	static constexpr idx_t BLOCK_TOKENS = 32;

	static bool *CachedFlags(ParserPackratEntry *block) {
		return reinterpret_cast<bool *>(block + BLOCK_TOKENS);
	}

	ArenaAllocator &arena;
	vector<ParserPackratEntry *> blocks;
	idx_t first_token;
	idx_t slot_count;
	idx_t row_count = 0;
};

} // namespace duckdb
