//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/compression/alp/alp_fetch.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/compression/alp/alp_scan.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/function/compression_function.hpp"

namespace duckdb {

template <class T>
struct AlpFetchState : public SegmentScanState {
	AlpFetchState(ColumnSegment &segment, ColumnFetchState &state)
	    : segment_data(state.GetOrInsertHandle(segment).GetDataMutable() + segment.GetBlockOffset()),
	      block_size(segment.GetBlockSize()), count(segment.count) {
		metadata_ptr = segment_data + Load<uint32_t>(segment_data);
	}

	T Fetch(idx_t row) const {
		const idx_t vector_index = row / AlpConstants::ALP_VECTOR_SIZE;
		const idx_t index = row % AlpConstants::ALP_VECTOR_SIZE;
		const auto data_byte_offset =
		    Load<uint32_t>(metadata_ptr - (vector_index + 1) * AlpConstants::METADATA_POINTER_SIZE);
		if (data_byte_offset >= block_size) {
			throw IOException(
			    "Corrupted ALP segment: stored data_byte_offset (%d) exceeds the segments block size (%d)",
			    data_byte_offset, block_size);
		}
		auto vector_ptr = segment_data + data_byte_offset;
		const auto exponent = Load<uint8_t>(vector_ptr);
		vector_ptr += AlpConstants::EXPONENT_SIZE;
		if (exponent == AlpConstants::UNCOMPRESSED_MODE_SENTINEL) {
			return Load<T>(vector_ptr + index * sizeof(T));
		}
		const auto factor = Load<uint8_t>(vector_ptr);
		vector_ptr += AlpConstants::FACTOR_SIZE;
		const auto exceptions_count = Load<uint16_t>(vector_ptr);
		vector_ptr += AlpConstants::EXCEPTIONS_COUNT_SIZE;
		const auto frame_of_reference = Load<uint64_t>(vector_ptr);
		vector_ptr += AlpConstants::FOR_SIZE;
		const auto bit_width = Load<uint8_t>(vector_ptr);
		vector_ptr += AlpConstants::BIT_WIDTH_SIZE;
		const idx_t vector_size =
		    MinValue<idx_t>(AlpConstants::ALP_VECTOR_SIZE, count - vector_index * AlpConstants::ALP_VECTOR_SIZE);
		if (exceptions_count > vector_size || factor > exponent || bit_width > sizeof(uint64_t) * 8) {
			throw IOException("Corrupted ALP segment: invalid vector header");
		}
		if (exceptions_count > 0) {
			const auto exceptions = vector_ptr + BitpackingPrimitives::GetRequiredSize(vector_size, bit_width);
			const auto positions = exceptions + exceptions_count * sizeof(T);
			idx_t low = 0;
			idx_t high = exceptions_count;
			while (low < high) {
				const idx_t mid = (low + high) / 2;
				if (Load<uint16_t>(positions + mid * AlpConstants::EXCEPTION_POSITION_SIZE) < index) {
					low = mid + 1;
				} else {
					high = mid;
				}
			}
			if (low < exceptions_count &&
			    Load<uint16_t>(positions + low * AlpConstants::EXCEPTION_POSITION_SIZE) == index) {
				return Load<T>(exceptions + low * sizeof(T));
			}
		}
		const auto encoded = BitpackingPrimitives::UnPackValue<uint64_t>(vector_ptr, index, bit_width) +
		                     frame_of_reference;
		return alp::AlpCompression<T, true>::DecodeValue(static_cast<int64_t>(encoded),
		                                                 alp::AlpEncodingIndices(exponent, factor));
	}

	data_ptr_t segment_data;
	data_ptr_t metadata_ptr;
	idx_t block_size;
	idx_t count;
};

template <class T>
void AlpFetchRow(ColumnSegment &segment, ColumnFetchState &state, row_t row_id, Vector &result, idx_t result_idx) {
	auto &fetch_state = state.GetOrInsertSegmentState<AlpFetchState<T>>(
	    segment, [&]() { return make_uniq<AlpFetchState<T>>(segment, state); });
	FlatVector::GetDataMutable<T>(result)[result_idx] = fetch_state.Fetch(UnsafeNumericCast<idx_t>(row_id));
}

} // namespace duckdb
