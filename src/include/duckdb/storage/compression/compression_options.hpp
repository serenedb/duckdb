//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/compression/compression_options.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/typedefs.hpp"

namespace duckdb {

//! Where a dictionary is shared: never, per row group, or per column.
enum class DictScope : uint8_t { NONE = 0, ROW_GROUP = 1, COLUMN = 2 };

//! How much effort a codec should spend. FLUSH is fast (duckdb always uses
//! this), MERGE is slow/best (iresearch consolidation may request it).
enum class CompressEffort : uint8_t { FLUSH = 0, MERGE = 1 };

//! Knobs a compression codec reads while analyzing/compressing a column.
//! Populated (priority) from per-column setting > force path > global settings
//! > defaults, and delivered via the analyze/compress context accessors.
struct CompressionOptions {
	//! ZSTD compression level; 0 = zstd default (3).
	int32_t zstd_level = 0;
	//! LZ4 compression level; 0 = lz4 default; >0 = HC level.
	int32_t lz4_level = 0;
	//! Whether dictionary encoding is allowed (off by default).
	bool use_dictionary = false;
	//! Scope of dictionary sharing.
	DictScope dict_scope = DictScope::NONE;
	//! Maximum dictionary size in bytes.
	idx_t max_dict_size = 112640;
	//! Effort hint; duckdb is always FLUSH, iresearch may set MERGE.
	CompressEffort effort = CompressEffort::FLUSH;
	//! FSST+ family form (raw FSSTPlusMode value): 0=fsst_plus, 1=dict_fsst_plus,
	//! 2=sorted_dict_fsst_plus, 3=fsst, 4=dict_fsst.
	uint8_t fsst_mode = 1;
};

} // namespace duckdb
