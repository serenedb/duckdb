#include "duckdb/storage/compression/dict_fsst/analyze.hpp"

#include "fsst.h"

namespace duckdb {
namespace dict_fsst {

//! Maximum size of an FSST symbol table, see fsst.h
static constexpr uint16_t FSST_SYMBOL_TABLE_SIZE = sizeof(duckdb_fsst_decoder_t);

//! Worst case is a single string filling an entire empty segment; returns the largest string length that fits.
static idx_t GetStringSizeLimit(const idx_t available_space, const bool fsst_encoded) {
	idx_t max_str_len = DictFSSTCompression::STRING_SIZE_LIMIT - 1;
	if (fsst_encoded) {
		//! In the worst case FSST may double the string length by prepending every byte with an exception
		max_str_len *= 2;
	}
	//! Dictionary contains NULL and the current string
	const bitpacking_width_t string_lengths_width = BitpackingPrimitives::MinimumBitWidth(max_str_len);
	const idx_t string_lengths_space = BitpackingPrimitives::GetRequiredSize(2, string_lengths_width);
	//! Dictionary stores only one valid string
	const bitpacking_width_t dict_indices_width = BitpackingPrimitives::MinimumBitWidth(1);
	const idx_t dict_indices_space = BitpackingPrimitives::GetRequiredSize(1, dict_indices_width);
	idx_t metadata_size = 0;
	metadata_size += AlignValue<idx_t>(sizeof(dict_fsst_compression_header_t));
	if (fsst_encoded) {
		//! As denoted in fsst.h
		metadata_size += 7;
	}
	//! Reserve maximum alignment padding for the variable length string
	metadata_size += sizeof(idx_t) - 1;
	if (fsst_encoded) {
		metadata_size += AlignValue<idx_t>(FSST_SYMBOL_TABLE_SIZE);
	}
	metadata_size += AlignValue<idx_t>(string_lengths_space);
	metadata_size += dict_indices_space;
	if (metadata_size >= available_space) {
		return 1;
	}
	idx_t max_string_size = available_space - metadata_size;
	if (fsst_encoded) {
		max_string_size = max_string_size / 2;
	}
	return MinValue(DictFSSTCompression::STRING_SIZE_LIMIT, max_string_size + 1);
}

DictFSSTAnalyzeState::DictFSSTAnalyzeState(BlockManager &block_manager) : AnalyzeState(block_manager) {
	fsst_string_size_limit = GetStringSizeLimit(info.GetBlockSize(), true);
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
		//! A segment cannot span multiple blocks: if a string cannot fit an empty segment even when
		//! FSST-encoded, DICT_FSST would fail on write (a crash with non-default block sizes), so decline
		//! DICT_FSST for this rowgroup. On the default block size this matches STRING_SIZE_LIMIT.
		if (str_len >= fsst_string_size_limit) {
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
