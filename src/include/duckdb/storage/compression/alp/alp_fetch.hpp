//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/compression/alp/alp_fetch.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/compression/alp/alp_scan.hpp"

#include "duckdb/common/likely.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/function/compression_function.hpp"

namespace duckdb {

template <class T>
struct AlpFetchState : public SegmentScanState {
	AlpFetchState(BufferHandle handle, ColumnSegment &segment) : scan_state(std::move(handle), segment) {
	}

	DUCKDB_NOINLINE T FetchValue(idx_t row) const {
		D_ASSERT(row < scan_state.count);
		const idx_t vector_index = row / AlpConstants::ALP_VECTOR_SIZE;
		const idx_t index = row % AlpConstants::ALP_VECTOR_SIZE;
		auto reader = scan_state.GetVectorReader(vector_index);
		const idx_t vector_size = MinValue<idx_t>(AlpConstants::ALP_VECTOR_SIZE,
		                                          scan_state.count - vector_index * AlpConstants::ALP_VECTOR_SIZE);
		const auto exponent = reader.template Read<AlpConstants::EXPONENT_TYPE>();
		if (exponent == AlpConstants::UNCOMPRESSED_MODE_SENTINEL) {
			return reader.template Get<T>(reader.Position() + index * sizeof(T));
		}
		if (exponent > AlpTypedConstants<T>::MAX_EXPONENT) {
			ThrowAlpExponentOutOfRange(exponent, AlpTypedConstants<T>::MAX_EXPONENT);
		}
		const auto factor = reader.template Read<AlpConstants::FACTOR_TYPE>();
		const auto exceptions_count = reader.template Read<AlpConstants::EXCEPTIONS_COUNT_TYPE>();
		const auto frame_of_reference = reader.template Read<AlpConstants::FRAME_OF_REFERENCE_TYPE>();
		const auto bit_width = reader.template Read<AlpConstants::BIT_WIDTH_TYPE>();
		if (exceptions_count > vector_size) {
			ThrowAlpExceptionCountOutOfRange(exceptions_count, vector_size);
		}
		if (factor > exponent) {
			ThrowAlpFactorOutOfRange(factor, exponent);
		}
		if (bit_width > AlpConstants::MAX_BIT_WIDTH) {
			ThrowAlpBitWidthOutOfRange(bit_width);
		}
		AlpConstants::ENCODED_VALUE_TYPE encoded = 0;
		if (bit_width > 0) {
			auto packed = reader.ReadBytes(BitpackingPrimitives::GetRequiredSize(vector_size, bit_width));
			encoded =
			    BitpackingPrimitives::UnPackValue<AlpConstants::ENCODED_VALUE_TYPE>(packed.data(), index, bit_width);
		}
		if (exceptions_count > 0) {
			auto exceptions = reader.ReadBytes(exceptions_count * sizeof(T));
			auto positions = reader.ReadBytes(exceptions_count * AlpConstants::EXCEPTION_POSITION_SIZE);
			idx_t found = exceptions_count;
			for (idx_t i = 0; i < exceptions_count; i++) {
				const auto position = Load<AlpConstants::EXCEPTION_POSITION_TYPE>(
				    positions.data() + i * AlpConstants::EXCEPTION_POSITION_SIZE);
				if (position >= vector_size) {
					ThrowAlpExceptionPositionOutOfRange(position, vector_size);
				}
				if (position == index) {
					found = i;
				}
			}
			if (found < exceptions_count) {
				return Load<T>(exceptions.data() + found * sizeof(T));
			}
		}
		return alp::AlpCompression<T, true>::DecodeValue(static_cast<int64_t>(encoded + frame_of_reference),
		                                                 alp::AlpEncodingIndices(exponent, factor));
	}

	AlpScanState<T> scan_state;
	//! The vector whose values the scan state holds decoded
	idx_t decoded_vector = DConstants::INVALID_INDEX;
	idx_t previous_row = DConstants::INVALID_INDEX;

	static constexpr idx_t DECODE_DISTANCE = AlpConstants::ALP_VECTOR_SIZE / 64;

	static bool Near(idx_t row, idx_t other) {
		return other != DConstants::INVALID_INDEX &&
		       row / AlpConstants::ALP_VECTOR_SIZE == other / AlpConstants::ALP_VECTOR_SIZE &&
		       (row > other ? row - other : other - row) <= DECODE_DISTANCE;
	}

	bool Alone(const idx_t *rows, idx_t i, idx_t count) {
		const auto previous = previous_row;
		previous_row = rows[i];
		return !Near(rows[i], previous) && (i + 1 == count || !Near(rows[i], rows[i + 1]));
	}
};

template <class T>
void AlpFetchRows(ColumnSegment &segment, ColumnFetchState &state, const idx_t *rows, idx_t count, Vector &result,
                  const SelectionVector &result_sel, idx_t result_offset) {
	using EXACT_TYPE = typename FloatingToExact<T>::TYPE;

	auto &fetch_state = state.GetOrCreateSegmentState<AlpFetchState<T>>(segment, [&]() {
		auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
		return make_uniq<AlpFetchState<T>>(buffer_manager.Pin(state.context, segment.GetBlockHandle()), segment);
	});
	auto &scan_state = fetch_state.scan_state;
	auto result_data = FlatVector::GetDataMutableUnsafe<EXACT_TYPE>(result);
	for (idx_t i = 0; i < count; i++) {
		auto vector_index = rows[i] / AlpConstants::ALP_VECTOR_SIZE;
		if (vector_index != fetch_state.decoded_vector) {
			if (fetch_state.Alone(rows, i, count)) {
				const auto value = fetch_state.FetchValue(rows[i]);
				memcpy(result_data + result_offset + result_sel.get_index(i), &value, sizeof(T));
				continue;
			}
			scan_state.total_value_count = vector_index * AlpConstants::ALP_VECTOR_SIZE;
			scan_state.LoadVector(scan_state.vector_state.decoded_values);
			fetch_state.decoded_vector = vector_index;
		}
		memcpy(result_data + result_offset + result_sel.get_index(i),
		       scan_state.vector_state.decoded_values + rows[i] % AlpConstants::ALP_VECTOR_SIZE, sizeof(T));
	}
}

} // namespace duckdb
