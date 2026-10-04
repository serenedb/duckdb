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
	ParserPackratCache(idx_t first_token, idx_t slot_count);

	optional_ptr<const ParserPackratEntry> Lookup(idx_t slot, idx_t token_index) const {
		auto offset = token_index - first_token;
		auto block = offset / BLOCK_TOKENS;
		if (slot >= slot_count || block >= blocks.size() || !blocks[block]) {
			return nullptr;
		}
		auto index = offset % BLOCK_TOKENS * slot_count + slot;
		if (!CachedFlags(blocks[block])[index]) {
			return nullptr;
		}
		return blocks[block][index];
	}
	void Store(idx_t slot, idx_t token_index, const ParserPackratEntry &entry);

private:
	static constexpr idx_t BLOCK_TOKENS = 32;

	bool *CachedFlags(ParserPackratEntry *block) const {
		return reinterpret_cast<bool *>(block + BLOCK_TOKENS * slot_count);
	}

	ArenaAllocator arena;
	vector<ParserPackratEntry *> blocks;
	idx_t first_token;
	idx_t slot_count;
};

} // namespace duckdb
