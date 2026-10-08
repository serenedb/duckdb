#include "duckdb/parser/peg/parser_packrat.hpp"

namespace duckdb {

ParserPackratCache::ParserPackratCache(ArenaAllocator &arena_p, idx_t first_token_p, idx_t slot_count_p)
    : arena(arena_p), first_token(first_token_p), slot_count(slot_count_p) {
}

void ParserPackratCache::Store(idx_t slot, idx_t token_index, const ParserPackratEntry &entry) {
	if (slot >= slot_count || token_index < first_token) {
		return;
	}
	auto offset = token_index - first_token;
	auto row = offset / BLOCK_TOKENS;
	if (row >= row_count) {
		row_count = row + 1;
		blocks.resize(row_count * slot_count);
	}
	auto &block = blocks[row * slot_count + slot];
	if (!block) {
		block = reinterpret_cast<ParserPackratEntry *>(
		    arena.AllocateAligned(BLOCK_TOKENS * (sizeof(ParserPackratEntry) + sizeof(bool))));
		memset(CachedFlags(block), 0, BLOCK_TOKENS * sizeof(bool));
	}
	auto index = offset % BLOCK_TOKENS;
	auto cached = CachedFlags(block);
	if (!cached[index]) {
		new (block + index) ParserPackratEntry(entry);
		cached[index] = true;
	}
}

} // namespace duckdb
