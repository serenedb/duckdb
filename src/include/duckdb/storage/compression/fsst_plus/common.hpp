#pragma once

#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/bitpacking.hpp"
#include "duckdb/common/types/string_type.hpp"

namespace duckdb {
namespace fsst_plus {

//! FSST+ sub-modes. All three share the SAME on-disk read path (a deduplicated
//! dictionary stored in FSST+ cleaved form + a bitpacked selection buffer); the
//! mode only records how the dictionary was built at compress time:
//!   - FSST_PLUS:             one entry per row (no dedup), row order preserved
//!                            through the selection buffer.
//!   - DICT_FSST_PLUS:        deduplicated dictionary (entry insertion order).
//!   - SORTED_DICT_FSST_PLUS: deduplicated + lexicographically sorted dictionary
//!                            (better prefix sharing); selection buffer still maps
//!                            each row to its entry so logical order is preserved.
enum class FSSTPlusMode : uint8_t {
	FSST_PLUS = 0,
	DICT_FSST_PLUS = 1,
	SORTED_DICT_FSST_PLUS = 2,
	COUNT //! Always last
};

//! Segment header. Kept POD + fixed size so it can be reinterpret_cast at the
//! start of the block. Widths are bitpacking_width_t stored as uint8_t.
typedef struct {
	FSSTPlusMode mode;
	uint8_t dictionary_indices_width; //! selection buffer (row -> entry idx)
	uint8_t prefix_id_width;          //! entry -> prefix table index
	uint8_t suffix_lengths_width;     //! entry -> encoded suffix byte length
	uint8_t prefix_lengths_width;     //! prefix table -> encoded prefix byte length
	uint8_t padding0;
	uint16_t padding1;
	uint32_t dict_count;         //! number of entries incl. entry 0 (NULL)
	uint32_t prefix_count;       //! number of distinct shared prefixes
	uint32_t symbol_table_size;  //! serialized FSST symbol table bytes
	uint32_t prefix_bytes_size;  //! concatenated encoded prefix bytes
	uint32_t suffix_bytes_size;  //! concatenated encoded suffix bytes
} fsst_plus_compression_header_t;

struct FSSTPlusCompression {
public:
	static constexpr uint16_t HEADER_SIZE = sizeof(fsst_plus_compression_header_t);
	//! Same cap dict_fsst uses: a single string this large opts the row group out.
	static constexpr idx_t STRING_SIZE_LIMIT = 16384;
	//! Entries with no shared prefix store prefix_id == prefix_count (a sentinel
	//! one past the last valid prefix index; prefix_id_width covers it).
	//! Scan below what the thesis calls a block; we cleave in runs of this many
	//! entries so the within-run TruncatedSort + DP stays cheap and prefixes are
	//! shared between lexicographically adjacent entries.
	static constexpr idx_t CLEAVE_RUN = 128;
	//! Longest shared prefix we will scan for (thesis max_prefix_size).
	static constexpr idx_t MAX_PREFIX = 255;
};

} // namespace fsst_plus
} // namespace duckdb
