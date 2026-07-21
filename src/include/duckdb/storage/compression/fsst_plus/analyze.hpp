#pragma once

#include "duckdb/storage/compression/fsst_plus/common.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {
namespace fsst_plus {

//===--------------------------------------------------------------------===//
// Analyze
//===--------------------------------------------------------------------===//
//! Unlike dict_fsst (which only accumulates counts and guesses total_len/2),
//! FSST+ deduplicates the column and runs the REAL cleaving/sizing pass in
//! FinalAnalyze so the analyze tournament only picks it when it actually wins.
struct FSSTPlusAnalyzeState : public AnalyzeState {
public:
	explicit FSSTPlusAnalyzeState(BlockManager &block_manager);

public:
	bool Analyze(const Vector &input);
	idx_t FinalAnalyze();

public:
	idx_t total_count = 0;
	bool contains_nulls = false;
	idx_t max_string_length = 0;
	//! Real measured serialized size (set by FinalAnalyze). Exposed so AUTO can be
	//! driven by honest sizing once FSST+ is opted into the tournament.
	idx_t measured_size = 0;

	//! Deduplicated dictionary entries (own their bytes) + membership set.
	unordered_map<string, uint32_t> seen;
	vector<string> uniq;
};

} // namespace fsst_plus
} // namespace duckdb
