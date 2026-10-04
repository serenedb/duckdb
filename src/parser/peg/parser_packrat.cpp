#include "duckdb/parser/peg/parser_packrat.hpp"

namespace duckdb {

ParserPackratCache::ParserPackratCache(idx_t first_token_p, idx_t slot_count_p)
    : arena(Allocator::DefaultAllocator()), first_token(first_token_p), slot_count(slot_count_p) {
}

void ParserPackratCache::Store(idx_t slot, idx_t token_index, const ParserPackratEntry &entry) {
	if (slot >= slot_count || token_index < first_token) {
		return;
	}
	auto offset = token_index - first_token;
	auto block = offset / BLOCK_TOKENS;
	if (block >= blocks.size()) {
		blocks.resize(block + 1);
	}
	if (!blocks[block]) {
		auto count = BLOCK_TOKENS * slot_count;
		auto data = reinterpret_cast<ParserPackratEntry *>(
		    arena.AllocateAligned(count * (sizeof(ParserPackratEntry) + sizeof(bool))));
		memset(CachedFlags(data), 0, count * sizeof(bool));
		blocks[block] = data;
	}
	auto index = offset % BLOCK_TOKENS * slot_count + slot;
	auto cached = CachedFlags(blocks[block]);
	if (!cached[index]) {
		new (blocks[block] + index) ParserPackratEntry(entry);
		cached[index] = true;
	}
}

} // namespace duckdb
