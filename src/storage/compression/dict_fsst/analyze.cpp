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
	UnifiedVectorFormat vdata;
	input.ToUnifiedFormat(vdata);
	const auto count = input.size();
	auto data = UnifiedVectorFormat::GetData<string_t>(vdata);
	idx_t total = 0;
	idx_t max_len = 0;
	for (idx_t i = 0; i < count; i++) {
		const auto idx = vdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(idx)) {
			contains_nulls = true;
			continue;
		}
		const idx_t str_len = data[idx].GetSize();
		total += str_len;
		max_len = MaxValue<idx_t>(max_len, str_len);
	}
	total_string_length += total;
	max_string_length = MaxValue<idx_t>(max_string_length, max_len);
	if (max_len >= string_size_limit) {
		return false;
	}
	total_count += count;
	return true;
}

idx_t DictFSSTAnalyzeState::FinalAnalyze() {
	return LossyNumericCast<idx_t>((double)total_string_length / 2.0);
}

} // namespace dict_fsst
} // namespace duckdb
