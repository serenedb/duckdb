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
	    : scan(BufferManager::GetBufferManager(segment.GetDatabase()).Pin(state.context, segment.GetBlockHandle()),
	           segment) {
	}

	T Fetch(idx_t row) const {
		D_ASSERT(row < scan.count);
		const idx_t vector_index = row / AlpConstants::ALP_VECTOR_SIZE;
		const idx_t index = row % AlpConstants::ALP_VECTOR_SIZE;
		auto reader = scan.GetVectorReader(vector_index);
		const idx_t vector_size =
		    MinValue<idx_t>(AlpConstants::ALP_VECTOR_SIZE, scan.count - vector_index * AlpConstants::ALP_VECTOR_SIZE);
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

	AlpScanState<T> scan;
};

template <class T>
void AlpFetchRow(ColumnSegment &segment, ColumnFetchState &state, row_t row_id, Vector &result, idx_t result_idx) {
	D_ASSERT(row_id >= 0);
	auto &fetch_state = state.GetOrInsertSegmentState<AlpFetchState<T>>(
	    segment, [&]() { return make_uniq<AlpFetchState<T>>(segment, state); });
	FlatVector::GetDataMutable<T>(result)[result_idx] = fetch_state.Fetch(NumericCast<idx_t>(row_id));
}

} // namespace duckdb
