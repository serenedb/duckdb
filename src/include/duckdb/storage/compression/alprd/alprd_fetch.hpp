//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/compression/alp/alp_fetch.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/compression/alprd/alprd_scan.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/function/compression_function.hpp"

namespace duckdb {

template <class T>
struct AlpRDFetchState : public SegmentScanState {
	using EXACT_TYPE = typename FloatingToExact<T>::TYPE;

	AlpRDFetchState(ColumnSegment &segment, ColumnFetchState &state)
	    : segment_data(state.GetOrInsertHandle(segment).GetDataMutable() + segment.GetBlockOffset()),
	      block_size(segment.GetBlockSize()), count(segment.count) {
		const auto metadata_offset = Load<uint32_t>(segment_data);
		if (segment.GetBlockOffset() + metadata_offset > block_size ||
		    segment.GetBlockOffset() + AlpRDConstants::HEADER_SIZE > block_size) {
			throw IOException("Corrupted ALPRD segment: metadata_offset value is corrupted");
		}
		metadata_ptr = segment_data + metadata_offset;
		auto header = segment_data + AlpRDConstants::METADATA_POINTER_SIZE;
		right_bit_width = Load<uint8_t>(header);
		header += AlpRDConstants::RIGHT_BIT_WIDTH_SIZE;
		left_bit_width = Load<uint8_t>(header);
		header += AlpRDConstants::LEFT_BIT_WIDTH_SIZE;
		const auto dictionary_size = Load<uint8_t>(header);
		header += AlpRDConstants::N_DICTIONARY_ELEMENTS_SIZE;
		if (dictionary_size > AlpRDConstants::MAX_DICTIONARY_SIZE ||
		    AlpRDConstants::HEADER_SIZE + dictionary_size * AlpRDConstants::DICTIONARY_ELEMENT_SIZE > metadata_offset) {
			throw IOException("Corrupted ALPRD segment: actual_dictionary_size is corrupted");
		}
		memset(left_parts_dict, 0, sizeof(left_parts_dict));
		memcpy(left_parts_dict, header, dictionary_size * AlpRDConstants::DICTIONARY_ELEMENT_SIZE);
	}

	EXACT_TYPE Fetch(idx_t row) const {
		const idx_t vector_index = row / AlpRDConstants::ALP_VECTOR_SIZE;
		const idx_t index = row % AlpRDConstants::ALP_VECTOR_SIZE;
		const auto data_byte_offset =
		    Load<uint32_t>(metadata_ptr - (vector_index + 1) * AlpRDConstants::METADATA_POINTER_SIZE);
		if (data_byte_offset >= block_size) {
			throw IOException(
			    "Corrupted ALPRD segment: stored data_byte_offset (%d) exceeds the segments block size (%d)",
			    data_byte_offset, block_size);
		}
		auto vector_ptr = segment_data + data_byte_offset;
		const auto exceptions_count = Load<uint16_t>(vector_ptr);
		vector_ptr += AlpRDConstants::EXCEPTIONS_COUNT_SIZE;
		if (exceptions_count == AlpRDConstants::UNCOMPRESSED_MODE_SENTINEL) {
			return Load<EXACT_TYPE>(vector_ptr + index * sizeof(EXACT_TYPE));
		}
		const idx_t vector_size =
		    MinValue<idx_t>(AlpRDConstants::ALP_VECTOR_SIZE, count - vector_index * AlpRDConstants::ALP_VECTOR_SIZE);
		if (exceptions_count > vector_size) {
			throw IOException("Corrupted ALPRD segment: exceptions_count exceeds the vector size");
		}
		const auto left_ptr = vector_ptr;
		const auto right_ptr = left_ptr + BitpackingPrimitives::GetRequiredSize(vector_size, left_bit_width);
		const auto right = BitpackingPrimitives::UnPackValue<EXACT_TYPE>(right_ptr, index, right_bit_width);
		uint16_t left = 0;
		bool exception = false;
		if (exceptions_count > 0) {
			const auto exceptions = right_ptr + BitpackingPrimitives::GetRequiredSize(vector_size, right_bit_width);
			const auto positions = exceptions + exceptions_count * AlpRDConstants::EXCEPTION_SIZE;
			idx_t low = 0;
			idx_t high = exceptions_count;
			while (low < high) {
				const idx_t mid = (low + high) / 2;
				if (Load<uint16_t>(positions + mid * AlpRDConstants::EXCEPTION_POSITION_SIZE) < index) {
					low = mid + 1;
				} else {
					high = mid;
				}
			}
			if (low < exceptions_count &&
			    Load<uint16_t>(positions + low * AlpRDConstants::EXCEPTION_POSITION_SIZE) == index) {
				left = Load<uint16_t>(exceptions + low * AlpRDConstants::EXCEPTION_SIZE);
				exception = true;
			}
		}
		if (!exception) {
			const auto code = BitpackingPrimitives::UnPackValue<uint16_t>(left_ptr, index, left_bit_width);
			if (code >= AlpRDConstants::MAX_DICTIONARY_SIZE) {
				throw IOException("Corrupted ALPRD segment: left part is outside of the dictionary");
			}
			left = left_parts_dict[code];
		}
		return (static_cast<EXACT_TYPE>(left) << right_bit_width) | right;
	}

	data_ptr_t segment_data;
	data_ptr_t metadata_ptr;
	idx_t block_size;
	idx_t count;
	uint8_t right_bit_width;
	uint8_t left_bit_width;
	uint16_t left_parts_dict[AlpRDConstants::MAX_DICTIONARY_SIZE];
};

template <class T>
void AlpRDFetchRow(ColumnSegment &segment, ColumnFetchState &state, row_t row_id, Vector &result, idx_t result_idx) {
	using EXACT_TYPE = typename FloatingToExact<T>::TYPE;
	auto &fetch_state = state.GetOrInsertSegmentState<AlpRDFetchState<T>>(
	    segment, [&]() { return make_uniq<AlpRDFetchState<T>>(segment, state); });
	FlatVector::GetDataMutableUnsafe<EXACT_TYPE>(result)[result_idx] =
	    fetch_state.Fetch(UnsafeNumericCast<idx_t>(row_id));
}

} // namespace duckdb
