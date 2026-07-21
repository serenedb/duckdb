#include "duckdb/storage/compression/fsst_plus/analyze.hpp"
#include "duckdb/storage/compression/fsst_plus/builder.hpp"

namespace duckdb {
namespace fsst_plus {

FSSTPlusAnalyzeState::FSSTPlusAnalyzeState(BlockManager &block_manager) : AnalyzeState(block_manager) {
}

bool FSSTPlusAnalyzeState::Analyze(const Vector &input) {
	for (auto entry : input.Values<string_t>()) {
		if (!entry.IsValid()) {
			contains_nulls = true;
			continue;
		}
		auto &str = entry.GetValue();
		auto str_len = str.GetSize();
		if (str_len >= FSSTPlusCompression::STRING_SIZE_LIMIT) {
			//! A single huge string opts the row group out of FSST+ (as dict_fsst does).
			return false;
		}
		if (str_len > max_string_length) {
			max_string_length = str_len;
		}
		string key(str.GetData(), str_len);
		auto it = seen.find(key);
		if (it == seen.end()) {
			seen.emplace(std::move(key), NumericCast<uint32_t>(uniq.size()));
			uniq.push_back(string(str.GetData(), str_len));
		}
	}
	total_count += input.size();
	return true;
}

idx_t FSSTPlusAnalyzeState::FinalAnalyze() {
	if (uniq.empty()) {
		//! Nothing (or all-NULL): let another method win.
		return DConstants::INVALID_INDEX;
	}
	vector<string_t> entries;
	entries.reserve(uniq.size());
	for (auto &s : uniq) {
		entries.emplace_back(s.data(), NumericCast<uint32_t>(s.size()));
	}
	CleavedDictionary dict;
	if (!BuildCleavedDictionary(dict, entries)) {
		//! Could not encode within budget -> opt out, another method is used.
		return DConstants::INVALID_INDEX;
	}
	idx_t dict_count = uniq.size() + 1; //! + NULL entry at index 0
	auto indices_width = BitpackingPrimitives::MinimumBitWidth(dict_count - 1);
	measured_size = CleavedDictionarySize(dict, total_count, indices_width);

	//! Force-only for now: keep FSST+ out of the AUTO tournament so existing AUTO
	//! defaults are unchanged (FOUNDATION_SPEC). We still run the REAL sizing pass
	//! above (measured_size), and force_compression still selects FSST+ because the
	//! checkpointer short-circuits on the forced method for any finite score.
	//! Flip this to `return measured_size;` once FSST+ is opted into AUTO.
	return DConstants::INVALID_INDEX - 1;
}

} // namespace fsst_plus
} // namespace duckdb
