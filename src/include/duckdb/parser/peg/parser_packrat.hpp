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
		auto cached = Find(slot, token_index);
		if (!cached || !cached->cached) {
			return nullptr;
		}
		return cached->entry;
	}
	void Store(idx_t slot, idx_t token_index, const ParserPackratEntry &entry);

private:
	struct CachedEntry {
		ParserPackratEntry entry;
		bool cached = false;
	};

	static constexpr idx_t BLOCK_TOKENS = 32;

	CachedEntry *Find(idx_t slot, idx_t token_index) const {
		auto offset = token_index - first_token;
		auto block = offset / BLOCK_TOKENS;
		if (slot >= slot_count || block >= blocks.size() || !blocks[block]) {
			return nullptr;
		}
		return &blocks[block][offset % BLOCK_TOKENS * slot_count + slot];
	}

	ArenaAllocator arena;
	vector<CachedEntry *> blocks;
	idx_t first_token;
	idx_t slot_count;
};

} // namespace duckdb
