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
		auto data = reinterpret_cast<CachedEntry *>(arena.AllocateAligned(count * sizeof(CachedEntry)));
		std::uninitialized_value_construct_n(data, count);
		blocks[block] = data;
	}
	auto &cached = blocks[block][offset % BLOCK_TOKENS * slot_count + slot];
	if (!cached.cached) {
		cached.entry = entry;
		cached.cached = true;
	}
}

} // namespace duckdb
