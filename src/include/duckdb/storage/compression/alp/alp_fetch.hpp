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
	AlpFetchState(BufferHandle handle, ColumnSegment &segment) : scan_state(std::move(handle), segment) {
	}

	AlpScanState<T> scan_state;
	//! The vector whose values the scan state holds decoded
	idx_t decoded_vector = DConstants::INVALID_INDEX;
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
		// Every vector can be located directly, so a row of another vector decodes just that vector
		auto vector_index = rows[i] / AlpConstants::ALP_VECTOR_SIZE;
		if (vector_index != fetch_state.decoded_vector) {
			scan_state.total_value_count = vector_index * AlpConstants::ALP_VECTOR_SIZE;
			scan_state.LoadVector(scan_state.vector_state.decoded_values);
			fetch_state.decoded_vector = vector_index;
		}
		memcpy(result_data + result_offset + result_sel.get_index(i),
		       scan_state.vector_state.decoded_values + rows[i] % AlpConstants::ALP_VECTOR_SIZE, sizeof(T));
	}
}

} // namespace duckdb
