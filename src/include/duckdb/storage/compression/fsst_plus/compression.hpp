#pragma once

#include "duckdb/storage/compression/fsst_plus/common.hpp"
#include "duckdb/storage/compression/fsst_plus/analyze.hpp"
#include "duckdb/storage/compression/standard_compression_state.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"
#include "duckdb/common/types/string_heap.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {
namespace fsst_plus {

//===--------------------------------------------------------------------===//
// Compress
//===--------------------------------------------------------------------===//
struct FSSTPlusCompressionState : public StandardCompressionState {
public:
	FSSTPlusCompressionState(ColumnDataCheckpointData &checkpoint_data, unique_ptr<FSSTPlusAnalyzeState> &&analyze);
	~FSSTPlusCompressionState() override;

public:
	void CreateEmptySegment();
	void Compress(const Vector &scan_vector);
	void FinalizeCompress();
	void Flush(bool final);

private:
	//! Conservative (pessimistic) estimate of the serialized size of the current
	//! segment; used to decide when to flush. Over-estimates encoded bytes (2x
	//! raw) + symbol table so the exact post-cleave size always fits.
	idx_t EstimatedSize(idx_t extra_raw, idx_t extra_entries) const;
	//! Build + write the accumulated dictionary to the current segment.
	idx_t Finalize();

public:
	StatsWriter<string_t> stats_writer;
	FSSTPlusMode mode = FSSTPlusMode::DICT_FSST_PLUS;

	//! accumulation for the current segment
	StringHeap entry_heap;
	unordered_map<string, uint32_t> dedup; //! content -> entry index (dict modes)
	vector<string_t> entries;              //! unique entries (point into entry_heap)
	vector<uint32_t> row_entry;            //! per row: 0 == NULL, else 1 + entry index
	idx_t running_raw_bytes = 0;
	idx_t tuple_count = 0;
	idx_t total_tuple_count = 0;

	unique_ptr<FSSTPlusAnalyzeState> analyze;
};

} // namespace fsst_plus
} // namespace duckdb
