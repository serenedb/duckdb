#pragma once

#include "duckdb/storage/compression/dict_fsst/common.hpp"
#include "duckdb/storage/arena_allocator.hpp"

namespace duckdb {

namespace dict_fsst {

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
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
	void Select(Vector &result, idx_t start, const SelectionVector &sel, idx_t sel_count);

	bool AllowDictionaryScan(idx_t scan_count);

private:
	//! Byte offset of entry `string_number` in dict_ptr. Forward reads extend the running offset (the cheap path
	//! every sequential scan takes); a backward re-seek materializes the full prefix sum once and is O(1) thereafter.
	uint32_t DecompressOffset(idx_t string_number);

	//! Reconstruct one entry from its parts: an optional shared prefix (when pid < prefix_count) followed by `len`
	//! encoded bytes at `src`. Covers every mode -- raw (DICTIONARY, decoder == nullptr), whole-string FSST (native),
	//! and prefix + suffix (plus) -- and decodes straight into an inline string_t when the whole segment inlines.
	string_t ReconstructEntry(ArenaAllocator &allocator, uint32_t pid, uint32_t len, const char *src);
	//! Reconstruct entry `entry_index` on demand, sourcing its parts from the full metadata arrays + DecompressOffset
	//! (the counterpart to the streamed materialize loop, which sources the same parts sequentially).
	string_t FetchEntry(ArenaAllocator &allocator, idx_t entry_index);

public:
	ColumnSegment &segment;
	BufferHandle owned_handle;
	optional_ptr<BufferHandle> handle;

	DictFSSTMode mode;
	uint32_t dict_count;
	bitpacking_width_t dictionary_indices_width;

	buffer_ptr<SelectionVector> sel_vec;
	idx_t sel_vec_size = 0;

	//===------------------------------------------------------------------===//
	// The generic entry model, shared by every mode.
	//
	// An entry is decoded from `dict_ptr + DecompressOffset(i)` for `entry_lengths[i]` encoded bytes, then
	// (optionally) prefixed by a shared prefix. Native modes (DICTIONARY / DICT_FSST / FSST_ONLY) store whole
	// strings, so they have no prefix indirection (prefix_count == 0); the plus modes cleave each entry into a
	// shared prefix + suffix, and the "entry bytes" are the suffixes. Nothing below is mode-specific except the
	// prefix pool, which is simply empty when prefix_count == 0.
	//===------------------------------------------------------------------===//

	//! Byte offset cursor into dict_ptr: a forward running sum of entry_lengths, promoted to a full prefix sum
	//! (decompress_offsets, null until then) on the first backward re-seek so random access is O(1) thereafter.
	uint32_t decompress_offset = 0;
	idx_t decompress_position = 0;
	unsafe_unique_array<uint32_t> decompress_offsets;

	//! Per-entry encoded byte length, dict_count values with index 0 the null slot (every mode, native and plus).
	//! Fully overwritten by UnPackBuffer, so allocated uninitialized -- a zero-initializing resize would just dirty
	//! cold cache lines per segment that the unpack immediately clobbers.
	unsafe_unique_array<uint32_t> entry_lengths;

	//! Start of the block (dictionary_header), the encoded entry byte source, and the row->entry selection buffer.
	data_ptr_t baseptr;
	data_ptr_t dict_ptr;
	data_ptr_t dictionary_indices_ptr;

	buffer_ptr<DictionaryEntry> dictionary;
	void *decoder = nullptr;
	bool all_values_inlined = false;

	//! Optional shared-prefix indirection (plus modes only; prefix_count == 0 for native). Entry i carries a
	//! prefix_ids[i]; ids >= prefix_count mean "no prefix". Every distinct prefix is FSST-decoded once at Initialize
	//! into one contiguous buffer so a per-entry reconstruct is a memcpy + one suffix decode.
	uint32_t prefix_count = 0;
	unsafe_unique_array<uint32_t> prefix_ids;
	unsafe_unique_array<char> prefix_decoded;
	//! Per-prefix {byte offset into prefix_decoded, decoded length}, one cache line per lookup in the hot reconstruct.
	struct PrefixSlot {
		uint32_t off;
		uint32_t len;
	};
	unsafe_unique_array<PrefixSlot> prefix_slots;

	unsafe_unique_array<bool> filter_result;
	//! How many dictionary entries pass the filter (valid when filter_result is set): 0 makes the
	//! whole segment a miss, dict_count makes every candidate row pass without the per-row walk
	idx_t filter_match_count = 0;
};

} // namespace dict_fsst

} // namespace duckdb
