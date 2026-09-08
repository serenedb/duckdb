#include "duckdb/storage/compression/dict_fsst/analyze.hpp"
#include "fsst.h"

namespace duckdb {
namespace dict_fsst {

namespace {

idx_t StringSizeLimit(idx_t block_size) {
	const idx_t overhead = DictFSSTCompression::PLUS_HEADER_SIZE + sizeof(duckdb_fsst_decoder_t) + 256;
	if (block_size <= overhead + 2) {
		return 1;
	}
	return MinValue<idx_t>(DictFSSTCompression::STRING_SIZE_LIMIT, (block_size - overhead) / 2);
}

} // namespace

DictFSSTAnalyzeState::DictFSSTAnalyzeState(BlockManager &block_manager)
    : AnalyzeState(block_manager), string_size_limit(StringSizeLimit(info.GetBlockSize())) {
}

bool DictFSSTAnalyzeState::Analyze(const Vector &input) {
	for (auto entry : input.Values<string_t>()) {
		if (!entry.IsValid()) {
			contains_nulls = true;
			continue;
		}
		auto &str = entry.GetValue();
		auto str_len = str.GetSize();
		total_string_length += str_len;
		if (str_len > max_string_length) {
			max_string_length = str_len;
		}
		if (str_len >= string_size_limit) {
			return false;
		}
	}
	total_count += input.size();
	return true;
}

idx_t DictFSSTAnalyzeState::FinalAnalyze() {
	return LossyNumericCast<idx_t>((double)total_string_length / 2.0);
}

} // namespace dict_fsst
} // namespace duckdb
