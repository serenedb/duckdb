#pragma once

#include "duckdb/storage/compression/fsst_plus/common.hpp"
#include "duckdb/storage/string_uncompressed.hpp"

namespace duckdb {
namespace fsst_plus {

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
//! Mirrors dict_fsst::CompressedStringScanState: the dictionary is reconstructed
//! ONCE at Initialize(), after which scans emit zero-copy DICTIONARY vectors
//! exactly like dict_fsst (read parity). The only FSST+ specific work is inside
//! the once-per-segment reconstruction loop (shared-prefix + suffix decode with a
//! per-distinct-prefix cache).
struct CompressedStringScanState : public SegmentScanState {
public:
	CompressedStringScanState(ColumnSegment &segment, BufferHandle &&handle_p)
	    : segment(segment), owned_handle(std::move(handle_p)), handle(owned_handle) {
	}
	CompressedStringScanState(ColumnSegment &segment, BufferHandle &handle_p)
	    : segment(segment), owned_handle(), handle(handle_p) {
	}
	~CompressedStringScanState() override;

public:
	void Initialize(bool initialize_dictionary = true);
	void ScanToFlatVector(Vector &result, idx_t result_offset, idx_t start, idx_t scan_count);
	void ScanToDictionaryVector(ColumnSegment &segment, Vector &result, idx_t result_offset, idx_t start,
	                            idx_t scan_count);
	const SelectionVector &GetSelVec(idx_t start, idx_t scan_count);
	bool AllowDictionaryScan(idx_t scan_count);

private:
	//! Reconstruct a single dictionary entry (1..dict_count-1) into `result`'s
	//! string allocator; used by fetch and the no-dictionary flat scan.
	string_t FetchEntry(Vector &result, idx_t entry_index);
	//! Decode distinct prefix `prefix_id` once, cached for the segment.
	const string &DecodePrefix(uint32_t prefix_id);

public:
	ColumnSegment &segment;
	BufferHandle owned_handle;
	optional_ptr<BufferHandle> handle;

	FSSTPlusMode mode;
	uint32_t dict_count = 0;   //! includes entry 0 (NULL)
	uint32_t prefix_count = 0;
	uint32_t entry_count = 0;  //! dict_count - 1
	bitpacking_width_t dictionary_indices_width = 0;

	//! unpacked metadata
	vector<uint32_t> prefix_lengths;   //! encoded prefix byte length per distinct prefix
	vector<uint32_t> prefix_offsets;   //! prefix-sum into prefix_bytes_ptr
	vector<uint32_t> prefix_ids;       //! entry -> prefix table idx (== prefix_count means none)
	vector<uint32_t> suffix_lengths;   //! encoded suffix byte length per entry
	vector<uint32_t> suffix_offsets;   //! prefix-sum into suffix_bytes_ptr

	//! per-segment decoded-prefix cache (design ruling: decode each prefix once)
	vector<string> prefix_cache;
	vector<bool> prefix_cached;

	buffer_ptr<SelectionVector> sel_vec;
	idx_t sel_vec_size = 0;

	data_ptr_t baseptr = nullptr;
	data_ptr_t selection_ptr = nullptr;
	data_ptr_t prefix_bytes_ptr = nullptr;
	data_ptr_t suffix_bytes_ptr = nullptr;

	buffer_ptr<DictionaryEntry> dictionary;
	void *decoder = nullptr;
	bool all_values_inlined = false;

	unsafe_unique_array<bool> filter_result;
	idx_t filter_match_count = 0;
};

} // namespace fsst_plus
} // namespace duckdb
