#include "duckdb/storage/compression/dict_fsst/analyze.hpp"

namespace duckdb {
namespace dict_fsst {

DictFSSTAnalyzeState::DictFSSTAnalyzeState(BlockManager &block_manager) : AnalyzeState(block_manager) {
}

bool DictFSSTAnalyzeState::Analyze(const Vector &input) {
	UnifiedVectorFormat vdata;
	input.ToUnifiedFormat(vdata);
	auto data = UnifiedVectorFormat::GetData<string_t>(vdata);
	const auto count = input.size();
	for (idx_t i = 0; i < count; i++) {
		auto idx = vdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(idx)) {
			contains_nulls = true;
			continue;
		}
		const auto &str = data[idx];
		auto str_len = str.GetSize();
		total_string_length += str_len;
		if (str_len > max_string_length) {
			max_string_length = str_len;
		}
		if (str_len >= DictFSSTCompression::STRING_SIZE_LIMIT) {
			//! This string is too long, we don't want to use DICT_FSST for this rowgroup
			return false;
		}
	}
	total_count += count;
	return true;
}

idx_t DictFSSTAnalyzeState::FinalAnalyze() {
	return LossyNumericCast<idx_t>((double)total_string_length / 2.0);
}

} // namespace dict_fsst
} // namespace duckdb
