#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/storage/compression/fsst_plus/decompression.hpp"
#include "duckdb/storage/compression/fsst_plus/builder.hpp"
#include "fsst.h"

namespace duckdb {
namespace fsst_plus {

CompressedStringScanState::~CompressedStringScanState() {
	delete reinterpret_cast<duckdb_fsst_decoder_t *>(decoder);
}

const string &CompressedStringScanState::DecodePrefix(uint32_t prefix_id) {
	if (!prefix_cached[prefix_id]) {
		auto enc_len = prefix_lengths[prefix_id];
		auto src = prefix_bytes_ptr + prefix_offsets[prefix_id];
		auto &out = prefix_cache[prefix_id];
		out.resize(size_t(enc_len) * 8 + 16);
		auto n = duckdb_fsst_decompress(reinterpret_cast<duckdb_fsst_decoder_t *>(decoder), enc_len, src, out.size(),
		                                reinterpret_cast<unsigned char *>(&out[0]));
		out.resize(n);
		prefix_cached[prefix_id] = true;
	}
	return prefix_cache[prefix_id];
}

string_t CompressedStringScanState::FetchEntry(Vector &result, idx_t entry_index) {
	uint32_t pid = prefix_ids[entry_index];
	uint32_t enc_suffix_len = suffix_lengths[entry_index];
	auto suffix_src = suffix_bytes_ptr + suffix_offsets[entry_index];

	idx_t prefix_len = 0;
	const char *prefix_data = nullptr;
	if (pid < prefix_count) {
		const string &p = DecodePrefix(pid);
		prefix_len = p.size();
		prefix_data = p.data();
	}
	// decode the suffix into a temporary, then assemble prefix + suffix.
	auto &allocator = StringVector::GetStringAllocator(result);
	idx_t decoded_cap = prefix_len + size_t(enc_suffix_len) * 8 + 16;
	auto target = StringVector::AllocateShrinkableBuffer(allocator, decoded_cap);
	if (prefix_len > 0) {
		memcpy(target, prefix_data, prefix_len);
	}
	idx_t suffix_len = 0;
	if (enc_suffix_len > 0) {
		suffix_len = duckdb_fsst_decompress(reinterpret_cast<duckdb_fsst_decoder_t *>(decoder), enc_suffix_len,
		                                    suffix_src, decoded_cap - prefix_len, target + prefix_len);
	}
	return StringVector::FinalizeShrinkableBuffer(allocator, target, decoded_cap, prefix_len + suffix_len);
}

void CompressedStringScanState::Initialize(bool initialize_dictionary) {
	baseptr = handle->GetDataMutable() + segment.GetBlockOffset();

	auto header_ptr = reinterpret_cast<fsst_plus_compression_header_t *>(baseptr);
	mode = header_ptr->mode;
	if (mode >= FSSTPlusMode::COUNT) {
		throw FatalException("FSST+ block written with unrecognized mode %d (highest known %d)",
		                     static_cast<uint8_t>(mode), static_cast<uint8_t>(FSSTPlusMode::COUNT));
	}
	dict_count = header_ptr->dict_count;
	entry_count = dict_count > 0 ? dict_count - 1 : 0;
	prefix_count = header_ptr->prefix_count;
	dictionary_indices_width = (bitpacking_width_t)header_ptr->dictionary_indices_width;
	auto prefix_lengths_width = (bitpacking_width_t)header_ptr->prefix_lengths_width;
	auto prefix_id_width = (bitpacking_width_t)header_ptr->prefix_id_width;
	auto suffix_lengths_width = (bitpacking_width_t)header_ptr->suffix_lengths_width;
	auto symbol_table_size = header_ptr->symbol_table_size;

	auto layout = FSSTPlusLayout::Compute(segment.count.load(), dict_count, prefix_count, dictionary_indices_width,
	                                      prefix_lengths_width, prefix_id_width, suffix_lengths_width, symbol_table_size,
	                                      header_ptr->prefix_bytes_size, header_ptr->suffix_bytes_size);
	const auto total_space = segment.GetBlockOffset() + layout.total;
	if (total_space > segment.GetBlockSize()) {
		throw IOException("Failed to scan FSST+ segment - out of range. Database file appears to be corrupted.");
	}

	selection_ptr = baseptr + layout.selection_dest;
	prefix_bytes_ptr = baseptr + layout.prefix_bytes_dest;
	suffix_bytes_ptr = baseptr + layout.suffix_bytes_dest;

	decoder = new duckdb_fsst_decoder_t;
	duckdb_fsst_import(reinterpret_cast<duckdb_fsst_decoder_t *>(decoder), baseptr + layout.symtab_dest);

	// unpack prefix lengths + prefix ids + suffix lengths
	constexpr auto GROUP = BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
	prefix_lengths.resize(AlignValue<uint32_t, GROUP>(MaxValue<uint32_t>(prefix_count, 1)));
	if (prefix_count > 0) {
		BitpackingPrimitives::UnPackBuffer<uint32_t>(data_ptr_cast(prefix_lengths.data()),
		                                             baseptr + layout.prefix_lengths_dest, prefix_count,
		                                             prefix_lengths_width);
	}
	prefix_ids.resize(AlignValue<uint32_t, GROUP>(MaxValue<uint32_t>(entry_count, 1)));
	suffix_lengths.resize(AlignValue<uint32_t, GROUP>(MaxValue<uint32_t>(entry_count, 1)));
	if (entry_count > 0) {
		BitpackingPrimitives::UnPackBuffer<uint32_t>(data_ptr_cast(prefix_ids.data()),
		                                             baseptr + layout.prefix_ids_dest, entry_count, prefix_id_width);
		BitpackingPrimitives::UnPackBuffer<uint32_t>(data_ptr_cast(suffix_lengths.data()),
		                                             baseptr + layout.suffix_lengths_dest, entry_count,
		                                             suffix_lengths_width);
	}
	// prefix-sum offsets
	prefix_offsets.resize(prefix_count);
	uint32_t acc = 0;
	for (uint32_t i = 0; i < prefix_count; i++) {
		prefix_offsets[i] = acc;
		acc += prefix_lengths[i];
	}
	suffix_offsets.resize(entry_count);
	acc = 0;
	for (uint32_t i = 0; i < entry_count; i++) {
		suffix_offsets[i] = acc;
		acc += suffix_lengths[i];
	}
	prefix_cache.assign(prefix_count, string());
	prefix_cached.assign(prefix_count, false);

	if (!initialize_dictionary) {
		return; // fetch path: reconstruct entries on demand
	}

	dictionary = DictionaryVector::CreateReusableDictionary(segment.GetType(), dict_count);
	auto &dict_data = dictionary->data;
	auto dict_child = FlatVector::GetDataMutable<string_t>(dict_data);
	auto &validity = FlatVector::ValidityMutable(dict_data);
	D_ASSERT(dict_count >= 1);
	validity.SetInvalid(0);
	for (uint32_t e = 1; e < dict_count; e++) {
		dict_child[e] = FetchEntry(dict_data, e - 1);
	}
}

const SelectionVector &CompressedStringScanState::GetSelVec(idx_t start, idx_t scan_count) {
	idx_t start_offset = start % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
	idx_t decompress_count = BitpackingPrimitives::RoundUpToAlgorithmGroupSize(scan_count + start_offset);
	if (!sel_vec || sel_vec_size < decompress_count) {
		sel_vec_size = decompress_count;
		sel_vec = make_buffer<SelectionVector>(decompress_count);
	}
	if (dictionary_indices_width == 0) {
		// dict_count == 1 (all NULL): every row maps to entry 0
		for (idx_t i = 0; i < decompress_count; i++) {
			sel_vec->set_index(i, 0);
		}
		return *sel_vec;
	}
	data_ptr_t src = &selection_ptr[((start - start_offset) * dictionary_indices_width) / 8];
	BitpackingPrimitives::UnPackBuffer<sel_t>(data_ptr_cast(sel_vec->data()), src, decompress_count,
	                                          dictionary_indices_width);
	if (start_offset != 0) {
		for (idx_t i = 0; i < scan_count; i++) {
			sel_vec->set_index(i, sel_vec->get_index(i + start_offset));
		}
	}
	return *sel_vec;
}

void CompressedStringScanState::ScanToFlatVector(Vector &result, idx_t result_offset, idx_t start, idx_t scan_count) {
	auto &selvec = GetSelVec(start, scan_count);
	auto result_data = FlatVector::Writer<string_t>(result, scan_count, result_offset);
	if (dictionary) {
		auto dictionary_values = FlatVector::GetData<string_t>(dictionary->data);
		for (idx_t i = 0; i < scan_count; i++) {
			auto string_number = selvec.get_index(i);
			if (string_number == 0) {
				result_data.WriteNull();
				continue;
			}
			result_data.WriteStringRef(dictionary_values[string_number]);
		}
	} else {
		for (idx_t i = 0; i < scan_count; i++) {
			auto string_number = selvec.get_index(i);
			if (string_number == 0) {
				result_data.WriteNull();
				continue;
			}
			result_data.WriteStringRef(FetchEntry(result, string_number - 1));
		}
	}
	result.Verify();
}

bool CompressedStringScanState::AllowDictionaryScan(idx_t scan_count) {
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

} // namespace fsst_plus
} // namespace duckdb
