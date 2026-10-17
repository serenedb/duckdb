//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/compression/alprd/alprd_fetch.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/compression/alprd/alprd_scan.hpp"

#include "duckdb/common/likely.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/function/compression_function.hpp"

namespace duckdb {

template <class T>
struct AlpRDFetchState : public SegmentScanState {
	AlpRDFetchState(BufferHandle handle, ColumnSegment &segment) : scan_state(std::move(handle), segment) {
	}

	using EXACT_TYPE = typename FloatingToExact<T>::TYPE;

	DUCKDB_NOINLINE EXACT_TYPE FetchValue(idx_t row) const {
		D_ASSERT(row < scan_state.count);
		const idx_t vector_index = row / AlpRDConstants::ALP_VECTOR_SIZE;
		const idx_t index = row % AlpRDConstants::ALP_VECTOR_SIZE;
		auto reader = scan_state.GetVectorReader(vector_index);
		const idx_t vector_size = MinValue<idx_t>(AlpRDConstants::ALP_VECTOR_SIZE,
		                                          scan_state.count - vector_index * AlpRDConstants::ALP_VECTOR_SIZE);
		const auto exceptions_count = reader.template Read<AlpRDConstants::EXCEPTIONS_COUNT_TYPE>();
		if (exceptions_count == AlpRDConstants::UNCOMPRESSED_MODE_SENTINEL) {
			return reader.template Get<EXACT_TYPE>(reader.Position() + index * sizeof(EXACT_TYPE));
		}
		if (exceptions_count > vector_size) {
			ThrowAlpRDExceptionCountOutOfRange(exceptions_count, vector_size);
		}
		const auto &vector_state = scan_state.vector_state;
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

	AlpRDScanState<T> scan_state;
	//! The vector whose values the scan state holds decoded
	idx_t decoded_vector = DConstants::INVALID_INDEX;
	idx_t previous_row = DConstants::INVALID_INDEX;

	static constexpr idx_t DECODE_DISTANCE = AlpRDConstants::ALP_VECTOR_SIZE / 64;

	static bool Near(idx_t row, idx_t other) {
		return other != DConstants::INVALID_INDEX &&
		       row / AlpRDConstants::ALP_VECTOR_SIZE == other / AlpRDConstants::ALP_VECTOR_SIZE &&
		       (row > other ? row - other : other - row) <= DECODE_DISTANCE;
	}

	bool Alone(const idx_t *rows, idx_t i, idx_t count) {
		const auto previous = previous_row;
		previous_row = rows[i];
		return !Near(rows[i], previous) && (i + 1 == count || !Near(rows[i], rows[i + 1]));
	}
};

template <class T>
void AlpRDFetchRows(ColumnSegment &segment, ColumnFetchState &state, const idx_t *rows, idx_t count, Vector &result,
                    const SelectionVector &result_sel, idx_t result_offset) {
	using EXACT_TYPE = typename FloatingToExact<T>::TYPE;

	auto &fetch_state = state.GetOrCreateSegmentState<AlpRDFetchState<T>>(segment, [&]() {
		auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
		return make_uniq<AlpRDFetchState<T>>(buffer_manager.Pin(state.context, segment.GetBlockHandle()), segment);
	});
	auto &scan_state = fetch_state.scan_state;
	auto result_data = FlatVector::GetDataMutableUnsafe<EXACT_TYPE>(result);
	for (idx_t i = 0; i < count; i++) {
		auto vector_index = rows[i] / AlpRDConstants::ALP_VECTOR_SIZE;
		if (vector_index != fetch_state.decoded_vector) {
			if (fetch_state.Alone(rows, i, count)) {
				result_data[result_offset + result_sel.get_index(i)] = fetch_state.FetchValue(rows[i]);
				continue;
			}
			scan_state.total_value_count = vector_index * AlpRDConstants::ALP_VECTOR_SIZE;
			scan_state.LoadVector(scan_state.vector_state.decoded_values);
			fetch_state.decoded_vector = vector_index;
		}
		result_data[result_offset + result_sel.get_index(i)] =
		    scan_state.vector_state.decoded_values[rows[i] % AlpRDConstants::ALP_VECTOR_SIZE];
	}
}

} // namespace duckdb
