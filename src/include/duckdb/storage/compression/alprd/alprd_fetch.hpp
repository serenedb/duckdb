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
	    : scan(BufferManager::GetBufferManager(segment.GetDatabase()).Pin(state.context, segment.GetBlockHandle()),
	           segment) {
	}

	EXACT_TYPE Fetch(idx_t row) const {
		D_ASSERT(row < scan.count);
		const idx_t vector_index = row / AlpRDConstants::ALP_VECTOR_SIZE;
		const idx_t index = row % AlpRDConstants::ALP_VECTOR_SIZE;
		auto reader = scan.GetVectorReader(vector_index);
		const idx_t vector_size = MinValue<idx_t>(AlpRDConstants::ALP_VECTOR_SIZE,
		                                          scan.count - vector_index * AlpRDConstants::ALP_VECTOR_SIZE);
		const auto exceptions_count = reader.template Read<AlpRDConstants::EXCEPTIONS_COUNT_TYPE>();
		if (exceptions_count == AlpRDConstants::UNCOMPRESSED_MODE_SENTINEL) {
			return reader.template Get<EXACT_TYPE>(reader.Position() + index * sizeof(EXACT_TYPE));
		}
		if (exceptions_count > vector_size) {
			ThrowAlpRDExceptionCountOutOfRange(exceptions_count, vector_size);
		}
		const auto &vector_state = scan.vector_state;
		auto left = reader.ReadBytes(BitpackingPrimitives::GetRequiredSize(vector_size, vector_state.left_bit_width));
		auto right = reader.ReadBytes(BitpackingPrimitives::GetRequiredSize(vector_size, vector_state.right_bit_width));
		const auto right_value =
		    BitpackingPrimitives::UnPackValue<EXACT_TYPE>(right.data(), index, vector_state.right_bit_width);
		if (exceptions_count > 0) {
			auto exceptions = reader.ReadBytes(exceptions_count * AlpRDConstants::EXCEPTION_SIZE);
			auto positions = reader.ReadBytes(exceptions_count * AlpRDConstants::EXCEPTION_POSITION_SIZE);
			idx_t found = exceptions_count;
			for (idx_t i = 0; i < exceptions_count; i++) {
				const auto position = Load<AlpRDConstants::EXCEPTION_POSITION_TYPE>(
				    positions.data() + i * AlpRDConstants::EXCEPTION_POSITION_SIZE);
				if (position >= vector_size) {
					ThrowAlpRDExceptionPositionOutOfRange(position, vector_size);
				}
				if (position == index) {
					found = i;
				}
			}
			if (found < exceptions_count) {
				const auto exception =
				    Load<AlpRDConstants::EXCEPTION_TYPE>(exceptions.data() + found * AlpRDConstants::EXCEPTION_SIZE);
				return (static_cast<EXACT_TYPE>(exception) << vector_state.right_bit_width) | right_value;
			}
		}
		const auto code = BitpackingPrimitives::UnPackValue<AlpRDConstants::DICTIONARY_ELEMENT_TYPE>(
		    left.data(), index, vector_state.left_bit_width);
		const auto left_value = vector_state.left_parts_dict[code];
		return (static_cast<EXACT_TYPE>(left_value) << vector_state.right_bit_width) | right_value;
	}

	AlpRDScanState<T> scan;
};

template <class T>
void AlpRDFetchRow(ColumnSegment &segment, ColumnFetchState &state, row_t row_id, Vector &result, idx_t result_idx) {
	using EXACT_TYPE = typename FloatingToExact<T>::TYPE;
	D_ASSERT(row_id >= 0);
	auto &fetch_state = state.GetOrInsertSegmentState<AlpRDFetchState<T>>(
	    segment, [&]() { return make_uniq<AlpRDFetchState<T>>(segment, state); });
	FlatVector::GetDataMutableUnsafe<EXACT_TYPE>(result)[result_idx] = fetch_state.Fetch(NumericCast<idx_t>(row_id));
}

} // namespace duckdb
