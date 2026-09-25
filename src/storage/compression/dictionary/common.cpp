#include "duckdb/storage/compression/dictionary/common.hpp"

#include <cstddef>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Helper Functions
//===--------------------------------------------------------------------===//
bool DictionaryCompression::HasEnoughSpace(idx_t current_count, idx_t index_count, idx_t dict_size,
                                           bitpacking_width_t packing_width, const idx_t block_size) {
	return RequiredSpace(current_count, index_count, dict_size, packing_width) <= block_size;
}

idx_t DictionaryCompression::RequiredSpace(idx_t current_count, idx_t index_count, idx_t dict_size,
                                           bitpacking_width_t packing_width) {
	idx_t base_space = DICTIONARY_HEADER_SIZE + dict_size;
	idx_t string_number_space = BitpackingPrimitives::GetRequiredSize(current_count, packing_width);
	idx_t index_space = index_count * sizeof(uint32_t);

	idx_t used_space = base_space + index_space + string_number_space;

	return used_space;
}

StringDictionaryContainer DictionaryCompression::GetDictionary(ColumnSegment &segment, BufferHandle &handle) {
	auto base_ptr = handle.GetDataMutable() + segment.GetBlockOffset();
	StringDictionaryContainer container;
	container.size = Load<uint32_t>(base_ptr + offsetof(dictionary_compression_header_t, dict_size));
	container.end = Load<uint32_t>(base_ptr + offsetof(dictionary_compression_header_t, dict_end));
	return container;
}

void DictionaryCompression::SetDictionary(ColumnSegment &segment, BufferHandle &handle,
                                          StringDictionaryContainer container) {
	auto base_ptr = handle.GetDataMutable() + segment.GetBlockOffset();
	Store<uint32_t>(container.size, base_ptr + offsetof(dictionary_compression_header_t, dict_size));
	Store<uint32_t>(container.end, base_ptr + offsetof(dictionary_compression_header_t, dict_end));
}

} // namespace duckdb
