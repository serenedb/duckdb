#pragma once

#include "duckdb/common/typedefs.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/common/bitpacking.hpp"
#include "duckdb/storage/string_uncompressed.hpp"

namespace duckdb {

//! The available compression modes. Modes 0-2 are the original duckdb-compatible on-disk formats and the ONLY modes
//! written to native duckdb storage. The FSST+ family (strings stored in cleaved shared-prefix/suffix form) is
//! serenedb-only: written exclusively to serenedb-owned storage (the __sdb_store facade + the iresearch .col
//! columnstore), never to a file that must round-trip with vanilla duckdb. It is numbered high (14-15) so that
//! values 3-13 stay free for a future vanilla-duckdb mode -- a duckdb file with such a mode is then rejected by the
//! reader's mode whitelist instead of being misread as an FSST+ segment.
//!
//! Deliberately in the top-level duckdb namespace (not dict_fsst) so generate_enum_util.py can emit its
//! ToChars/FromString -- that generated pair IS the DictFSSTMode <-> string mapping used by the force_dict_fsst_mode
//! setting and by pragma_storage_info, so there is no hand-written table to drift.
enum class DictFSSTMode : uint8_t {
	DICTIONARY = 0,
	DICT_FSST = 1,
	FSST_ONLY = 2,
	DICT_FSST_PLUS = 14, //! dedup + fsst + shared prefix (dictionary sorted before cleaving)
	FSST_PLUS = 15,      //! no dedup + fsst + shared prefix (one entry per row)
	COUNT                //! sentinel for "auto / no forced mode"; never written to disk
};

namespace dict_fsst {

//! True for the serenedb-only cleaved (FSST+) modes.
inline bool IsPlusMode(DictFSSTMode mode) {
	return mode == DictFSSTMode::DICT_FSST_PLUS || mode == DictFSSTMode::FSST_PLUS;
}

//! True for the native (vanilla-duckdb) modes.
inline bool IsNativeMode(DictFSSTMode mode) {
	return mode == DictFSSTMode::DICTIONARY || mode == DictFSSTMode::DICT_FSST || mode == DictFSSTMode::FSST_ONLY;
}

//! Base segment header, byte-identical to the original for modes 0-2. `mode` (offset 8) is the dispatch field for
//! all modes, so a reader can tell a plus segment from a native one before it looks at anything else. For plus modes
//! `dict_size`/`string_lengths_width` are unused (0); `dict_count`, `dictionary_indices_width` (selection width) and
//! `symbol_table_size` keep their meaning, and a dict_fsst_plus_header_t follows immediately after (see below).
typedef struct {
	uint32_t dict_size;
	uint32_t dict_count;
	DictFSSTMode mode;
	uint8_t string_lengths_width;
	uint8_t dictionary_indices_width;
	uint32_t symbol_table_size;
} dict_fsst_compression_header_t;

//! Extension header for the plus modes only. Placed right after the (aligned) base header; never present for modes
//! 0-2, so it cannot perturb their layout.
typedef struct {
	uint32_t prefix_count;      //! number of distinct shared prefixes
	uint32_t prefix_bytes_size; //! concatenated encoded prefix bytes
	uint32_t suffix_bytes_size; //! concatenated encoded suffix bytes
	uint8_t prefix_id_width;
	uint8_t prefix_lengths_width;
	uint8_t suffix_lengths_width;
	uint8_t padding;
} dict_fsst_plus_header_t;

struct DictFSSTCompression {
public:
	//! Dictionary header size at the beginning of the string segment (offset + length)
	static constexpr uint16_t DICTIONARY_HEADER_SIZE = sizeof(dict_fsst_compression_header_t);
	static constexpr idx_t STRING_SIZE_LIMIT = 16384;
	//! Where the plus-mode payload begins: the (8-byte-aligned) base header followed by the plus extension header.
	static constexpr idx_t PLUS_HEADER_SIZE =
	    ((sizeof(dict_fsst_compression_header_t) + 7) / 8 * 8) + sizeof(dict_fsst_plus_header_t);
};

//! Byte offsets (relative to the block start) of every region in a plus segment. Computed once and used by BOTH the
//! writer (Finalize) and the reader (Initialize) so the two can never disagree. `dict_count` includes entry 0 (NULL);
//! entry_count == dict_count - 1.
struct DictFSSTPlusLayout {
	idx_t selection_space = 0;
	idx_t prefix_lengths_space = 0;
	idx_t prefix_ids_space = 0;
	idx_t suffix_lengths_space = 0;

	idx_t selection_dest = 0;
	idx_t symtab_dest = 0;
	idx_t prefix_lengths_dest = 0;
	idx_t prefix_bytes_dest = 0;
	idx_t prefix_ids_dest = 0;
	idx_t suffix_lengths_dest = 0;
	idx_t suffix_bytes_dest = 0;
	idx_t total = 0;

	static DictFSSTPlusLayout Compute(idx_t tuple_count, idx_t dict_count, idx_t prefix_count,
	                                  bitpacking_width_t indices_width, bitpacking_width_t prefix_lengths_width,
	                                  bitpacking_width_t prefix_id_width, bitpacking_width_t suffix_lengths_width,
	                                  idx_t symbol_table_size, idx_t prefix_bytes, idx_t suffix_bytes) {
		DictFSSTPlusLayout l;
		// prefix_ids / suffix_lengths carry a leading null slot at index 0 (like native's dictionary), so they hold
		// dict_count values: the selection value indexes them directly and the read path is mode-agnostic.
		l.selection_space = BitpackingPrimitives::GetRequiredSize(tuple_count, indices_width);
		l.prefix_lengths_space = BitpackingPrimitives::GetRequiredSize(prefix_count, prefix_lengths_width);
		l.prefix_ids_space = BitpackingPrimitives::GetRequiredSize(dict_count, prefix_id_width);
		l.suffix_lengths_space = BitpackingPrimitives::GetRequiredSize(dict_count, suffix_lengths_width);

		l.selection_dest = AlignValue<idx_t>(DictFSSTCompression::PLUS_HEADER_SIZE);
		l.symtab_dest = AlignValue<idx_t>(l.selection_dest + l.selection_space);
		l.prefix_lengths_dest = AlignValue<idx_t>(l.symtab_dest + symbol_table_size);
		l.prefix_bytes_dest = AlignValue<idx_t>(l.prefix_lengths_dest + l.prefix_lengths_space);
		l.prefix_ids_dest = AlignValue<idx_t>(l.prefix_bytes_dest + prefix_bytes);
		l.suffix_lengths_dest = AlignValue<idx_t>(l.prefix_ids_dest + l.prefix_ids_space);
		l.suffix_bytes_dest = AlignValue<idx_t>(l.suffix_lengths_dest + l.suffix_lengths_space);
		l.total = l.suffix_bytes_dest + suffix_bytes;
		return l;
	}
};

} // namespace dict_fsst

} // namespace duckdb
