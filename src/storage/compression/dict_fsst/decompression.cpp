#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/storage/compression/dict_fsst/decompression.hpp"
#include "fsst.h"
#include "duckdb/common/fsst.hpp"

namespace duckdb {
namespace dict_fsst {

CompressedStringScanState::~CompressedStringScanState() {
	delete reinterpret_cast<duckdb_fsst_decoder_t *>(decoder);
}

string_t CompressedStringScanState::FetchEntry(ArenaAllocator &allocator, idx_t entry_index) {
	uint32_t pid = prefix_count > 0 ? prefix_ids[entry_index] : 0;
	return ReconstructEntry(allocator, pid, entry_lengths[entry_index],
	                        char_ptr_cast(dict_ptr + DecompressOffset(entry_index)));
}

string_t CompressedStringScanState::ReconstructEntry(ArenaAllocator &allocator, uint32_t pid, uint32_t len,
                                                     const char *src) {
	if (pid >= prefix_count) {
		if (len == 0) {
			return string_t(nullptr, 0);
		}
		if (!decoder) {
			return string_t(src, len);
		}
		return all_values_inlined ? FSSTPrimitives::DecompressInlinedValue(decoder, src, len)
		                          : FSSTPrimitives::DecompressValue(decoder, allocator, src, len);
	}
	auto slot = prefix_slots[pid];
	auto prefix = prefix_decoded.get() + slot.off;
	idx_t prefix_len = slot.len;
	return all_values_inlined
	           ? FSSTPrimitives::DecompressInlinedValueWithPrefix(decoder, prefix, prefix_len, src, len)
	           : FSSTPrimitives::DecompressValueWithPrefix(decoder, allocator, prefix, prefix_len, src, len);
}

uint32_t CompressedStringScanState::DecompressOffset(idx_t string_number) {
	if (decompress_offsets) {
		// Prefix sum already materialized (a backward re-seek happened earlier): O(1) random access.
		return decompress_offsets[string_number];
	}
	if (string_number >= decompress_position) {
		// Forward read: extend the running offset. This is the only path sequential/native scans take.
		for (; decompress_position < string_number; decompress_position++) {
			decompress_offset += entry_lengths[decompress_position];
		}
		return decompress_offset;
	}
	// Backward re-seek (a warm-kept lookup cursor repositioned to an earlier row): materialize the full
	// prefix sum once so this and every later lookup is O(1), instead of re-walking from the start.
	decompress_offsets = make_unsafe_uniq_array<uint32_t>(dict_count + 1);
	uint32_t acc = 0;
	for (uint32_t i = 0; i < dict_count; i++) {
		decompress_offsets[i] = acc;
		acc += entry_lengths[i];
	}
	decompress_offsets[dict_count] = acc;
	return decompress_offsets[string_number];
}

void CompressedStringScanState::Initialize(bool initialize_dictionary) {
	baseptr = handle->GetDataMutable() + segment.GetBlockOffset();

	// Load header values
	auto header_ptr = reinterpret_cast<dict_fsst_compression_header_t *>(baseptr);
	mode = header_ptr->mode;
	if (!IsNativeMode(mode) && !IsPlusMode(mode)) {
		throw FatalException("This block was written with a dict_fsst mode not recognized by this version of SereneDB: "
		                     "%d",
		                     static_cast<uint8_t>(mode));
	}

	dict_count = header_ptr->dict_count;
	auto symbol_table_size = header_ptr->symbol_table_size;

	dictionary_indices_width =
	    (bitpacking_width_t)(Load<uint8_t>(data_ptr_cast(&header_ptr->dictionary_indices_width)));

	static constexpr auto GROUP = BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
	const bool plus = IsPlusMode(mode);

	data_ptr_t symtab_ptr = nullptr;
	data_ptr_t entry_lengths_src = nullptr;
	data_ptr_t prefix_ids_src = nullptr;
	data_ptr_t prefix_lengths_src = nullptr;
	data_ptr_t prefix_bytes_src = nullptr;
	bitpacking_width_t entry_lengths_width = 0;
	bitpacking_width_t prefix_lengths_width = 0;
	bitpacking_width_t prefix_id_width = 0;
	idx_t prefix_decoded_cap = 0;
	if (plus) {
		auto ext = reinterpret_cast<dict_fsst_plus_header_t *>(
		    baseptr + AlignValue<idx_t>(sizeof(dict_fsst_compression_header_t)));
		prefix_count = ext->prefix_count;
		prefix_decoded_cap = size_t(ext->prefix_bytes_size) * 8 + 8;
		prefix_lengths_width = (bitpacking_width_t)(Load<uint8_t>(data_ptr_cast(&ext->prefix_lengths_width)));
		prefix_id_width = (bitpacking_width_t)(Load<uint8_t>(data_ptr_cast(&ext->prefix_id_width)));
		entry_lengths_width = (bitpacking_width_t)(Load<uint8_t>(data_ptr_cast(&ext->suffix_lengths_width)));
		auto layout = DictFSSTPlusLayout::Compute(
		    segment.count.load(), dict_count, prefix_count, dictionary_indices_width, prefix_lengths_width,
		    prefix_id_width, entry_lengths_width, symbol_table_size, ext->prefix_bytes_size, ext->suffix_bytes_size);
		if (segment.GetBlockOffset() + layout.total > segment.GetBlockSize()) {
			throw IOException(
			    "Failed to scan dictionary string - index was out of range. Database file appears to be corrupted.");
		}
		symtab_ptr = baseptr + layout.symtab_dest;
		dictionary_indices_ptr = baseptr + layout.selection_dest;
		dict_ptr = baseptr + layout.suffix_bytes_dest;
		entry_lengths_src = baseptr + layout.suffix_lengths_dest;
		prefix_ids_src = baseptr + layout.prefix_ids_dest;
		prefix_lengths_src = baseptr + layout.prefix_lengths_dest;
		prefix_bytes_src = baseptr + layout.prefix_bytes_dest;
	} else {
		prefix_count = 0;
		auto dictionary_size = header_ptr->dict_size;
		entry_lengths_width = (bitpacking_width_t)(Load<uint8_t>(data_ptr_cast(&header_ptr->string_lengths_width)));
		auto entry_lengths_space = BitpackingPrimitives::GetRequiredSize(dict_count, entry_lengths_width);
		auto dictionary_indices_space =
		    BitpackingPrimitives::GetRequiredSize(segment.count.load(), dictionary_indices_width);
		auto dictionary_dest = AlignValue<idx_t>(DictFSSTCompression::DICTIONARY_HEADER_SIZE);
		auto symbol_table_dest = AlignValue<idx_t>(dictionary_dest + dictionary_size);
		auto entry_lengths_dest = AlignValue<idx_t>(symbol_table_dest + symbol_table_size);
		auto dictionary_indices_dest = AlignValue<idx_t>(entry_lengths_dest + entry_lengths_space);
		if (segment.GetBlockOffset() + dictionary_indices_dest + dictionary_indices_space > segment.GetBlockSize()) {
			throw IOException(
			    "Failed to scan dictionary string - index was out of range. Database file appears to be corrupted.");
		}
		symtab_ptr = baseptr + symbol_table_dest;
		dict_ptr = data_ptr_cast(baseptr + dictionary_dest);
		dictionary_indices_ptr = data_ptr_cast(baseptr + dictionary_indices_dest);
		entry_lengths_src = data_ptr_cast(baseptr + entry_lengths_dest);
	}

	if (mode != DictFSSTMode::DICTIONARY) {
		decoder = new duckdb_fsst_decoder_t;
		auto ret = duckdb_fsst_import(reinterpret_cast<duckdb_fsst_decoder_t *>(decoder), symtab_ptr);
		(void)(ret);
		D_ASSERT(ret != 0);
	}

	const bool materialize =
	    initialize_dictionary && mode != DictFSSTMode::FSST_ONLY && mode != DictFSSTMode::FSST_PLUS;

	const auto &stats = segment.GetStats();
	all_values_inlined = stats.GetStatsType() == StatisticsType::STRING_STATS &&
	                     StringStats::HasMaxStringLength(stats) &&
	                     StringStats::MaxStringLength(stats) <= string_t::INLINE_LENGTH;

	if (prefix_count > 0) {
		auto fsst_decoder = reinterpret_cast<duckdb_fsst_decoder_t *>(decoder);
		prefix_slots = make_unsafe_uniq_array<PrefixSlot>(prefix_count);
		prefix_decoded = make_unsafe_uniq_array<char>(prefix_decoded_cap);
		auto pd = reinterpret_cast<unsigned char *>(prefix_decoded.get());
		uint32_t enc_len_group[GROUP];
		idx_t enc_off = 0;
		idx_t out = 0;
		for (uint32_t g_base = 0; g_base < prefix_count; g_base += GROUP) {
			BitpackingPrimitives::UnPackBlock<uint32_t>(
			    data_ptr_cast(enc_len_group), prefix_lengths_src + (size_t(g_base) * prefix_lengths_width) / 8,
			    prefix_lengths_width);
			uint32_t g_n = MinValue<uint32_t>(GROUP, prefix_count - g_base);
			for (uint32_t j = 0; j < g_n; j++) {
				auto el = enc_len_group[j];
				auto n = duckdb_fsst_decompress(fsst_decoder, el, prefix_bytes_src + enc_off, prefix_decoded_cap - out,
				                                pd + out);
				prefix_slots[g_base + j] = {NumericCast<uint32_t>(out), NumericCast<uint32_t>(n)};
				out += n;
				enc_off += el;
			}
		}
	}

	if (!materialize) {
		entry_lengths = make_unsafe_uniq_array<uint32_t>(AlignValue<uint32_t, GROUP>(dict_count));
		BitpackingPrimitives::UnPackBuffer<uint32_t>(data_ptr_cast(entry_lengths.get()), entry_lengths_src, dict_count,
		                                             entry_lengths_width);
		if (prefix_count > 0) {
			prefix_ids = make_unsafe_uniq_array<uint32_t>(AlignValue<uint32_t, GROUP>(dict_count));
			BitpackingPrimitives::UnPackBuffer<uint32_t>(data_ptr_cast(prefix_ids.get()), prefix_ids_src, dict_count,
			                                             prefix_id_width);
		}
		return;
	}

	dictionary = DictionaryVector::CreateReusableDictionary(segment.GetType(), dict_count);
	auto &dict_data = dictionary->data;
	auto dict_child_data = FlatVector::GetDataMutable<string_t>(dict_data);
	auto &validity = FlatVector::ValidityMutable(dict_data);
	D_ASSERT(dict_count >= 1);
	validity.SetInvalid(0);

	auto &allocator = StringVector::GetStringAllocator(dict_data);
	uint32_t pid_group[GROUP];
	uint32_t slen_group[GROUP];
	idx_t offset = 0;
	for (uint32_t g_base = 0; g_base < dict_count; g_base += GROUP) {
		BitpackingPrimitives::UnPackBlock<uint32_t>(data_ptr_cast(slen_group),
		                                            entry_lengths_src + (size_t(g_base) * entry_lengths_width) / 8,
		                                            entry_lengths_width);
		if (prefix_count > 0) {
			BitpackingPrimitives::UnPackBlock<uint32_t>(
			    data_ptr_cast(pid_group), prefix_ids_src + (size_t(g_base) * prefix_id_width) / 8, prefix_id_width);
		}
		uint32_t g_n = MinValue<uint32_t>(GROUP, dict_count - g_base);
		for (uint32_t j = 0; j < g_n; j++) {
			uint32_t len = slen_group[j];
			auto src = char_ptr_cast(dict_ptr + offset);
			offset += len;
			uint32_t pid = prefix_count > 0 ? pid_group[j] : 0;
			dict_child_data[g_base + j] = ReconstructEntry(allocator, pid, len, src);
		}
	}
}

const SelectionVector &CompressedStringScanState::GetSelVec(idx_t start, idx_t scan_count) {
	D_ASSERT(mode != DictFSSTMode::FSST_ONLY && mode != DictFSSTMode::FSST_PLUS);
	// Handling non-bitpacking-group-aligned start values;
	idx_t start_offset = start % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;

	// We will scan in blocks of BITPACKING_ALGORITHM_GROUP_SIZE, so we may scan some extra values.
	idx_t decompress_count = BitpackingPrimitives::RoundUpToAlgorithmGroupSize(scan_count + start_offset);

	if (!sel_vec || sel_vec_size < decompress_count) {
		sel_vec_size = decompress_count;
		sel_vec = make_buffer<SelectionVector>(decompress_count);
	}

	data_ptr_t sel_buf_src = &dictionary_indices_ptr[((start - start_offset) * dictionary_indices_width) / 8];
	sel_t *sel_vec_ptr = sel_vec->data();
	BitpackingPrimitives::UnPackBuffer<sel_t>(data_ptr_cast(sel_vec_ptr), sel_buf_src, decompress_count,
	                                          dictionary_indices_width);

	if (start_offset != 0) {
		for (idx_t i = 0; i < scan_count; i++) {
			sel_vec->set_index(i, sel_vec->get_index(i + start_offset));
		}
	}

	return *sel_vec;
}

void CompressedStringScanState::ScanToFlatVector(Vector &result, idx_t result_offset, idx_t start, idx_t scan_count) {
	auto result_data = FlatVector::Writer<string_t>(result, scan_count, result_offset);
	if (mode == DictFSSTMode::FSST_ONLY || mode == DictFSSTMode::FSST_PLUS) {
		auto &allocator = StringVector::GetStringAllocator(result);
		//! (index 0 is reserved for NULL, which we don't have in this mode)
		const idx_t start_offset = start + 1;
		for (idx_t i = 0; i < scan_count; i++) {
			result_data.WriteStringRef(FetchEntry(allocator, start_offset + i));
		}
		result.Verify();
		return;
	}

	// Create a decompression buffer of sufficient size if we don't already have one.
	auto &selvec = GetSelVec(start, scan_count);
	if (dictionary) {
		// We have prepared the full dictionary, we can reference these strings directly
		auto dictionary_values = FlatVector::GetData<string_t>(dictionary->data);
		for (idx_t i = 0; i < scan_count; i++) {
			// Lookup dict offset in index buffer
			auto string_number = selvec.get_index(i);
			if (string_number == 0) {
				result_data.WriteNull();
				continue;
			}
			result_data.WriteStringRef(dictionary_values[string_number]);
		}
	} else {
		auto &allocator = StringVector::GetStringAllocator(result);
		for (idx_t i = 0; i < scan_count; i++) {
			// Lookup dict offset in index buffer
			auto string_number = selvec.get_index(i);
			if (string_number == 0) {
				result_data.WriteNull();
				continue;
			}
			result_data.WriteStringRef(FetchEntry(allocator, string_number));
		}
	}
	result.Verify();
}

void CompressedStringScanState::Select(Vector &result, idx_t start, const SelectionVector &sel, idx_t sel_count) {
	D_ASSERT(!dictionary);
	D_ASSERT(mode == DictFSSTMode::FSST_ONLY || mode == DictFSSTMode::FSST_PLUS);
	idx_t start_offset = start + 1;
	auto result_data = FlatVector::Writer<string_t>(result, sel_count);
	auto &allocator = StringVector::GetStringAllocator(result);
	for (idx_t i = 0; i < sel_count; i++) {
		// Lookup dict offset in index buffer
		auto string_number = start_offset + sel.get_index(i);
		result_data.WriteStringRef(FetchEntry(allocator, string_number));
	}
}

bool CompressedStringScanState::AllowDictionaryScan(idx_t scan_count) {
	if (mode == DictFSSTMode::FSST_ONLY || mode == DictFSSTMode::FSST_PLUS) {
		return false;
	}
	if (scan_count != STANDARD_VECTOR_SIZE) {
		return false;
	}
	if (!dictionary) {
		return false;
	}
	return true;
}

void CompressedStringScanState::ScanToDictionaryVector(ColumnSegment &segment, Vector &result, idx_t result_offset,
                                                       idx_t start, idx_t scan_count) {
	D_ASSERT(scan_count == STANDARD_VECTOR_SIZE);
	D_ASSERT(result_offset == 0);

	auto &selvec = GetSelVec(start, scan_count);
	result.Dictionary(dictionary, selvec, scan_count);
	result.Verify();
}

} // namespace dict_fsst
} // namespace duckdb
