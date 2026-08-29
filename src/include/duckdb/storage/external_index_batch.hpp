//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/external_index_batch.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

//! One batch of rows handed to an external index, which tokenizes it on worker threads and so keeps
//! it past the loop that produced it. Producers scan `data` in full table layout.
struct ExternalIndexBatch {
	DataChunk data;
	Vector row_ids;

	ExternalIndexBatch() : row_ids(LogicalType::ROW_TYPE) {
	}

	//! Call once `data` has been scanned. Flattens what a reader on another thread could not
	//! safely share with the producer, and numbers the rows from `row_start`.
	void Finalize(row_t row_start) {
		for (idx_t i = 0; i < data.ColumnCount(); i++) {
			auto &vec = data.data[i];
			switch (vec.GetVectorType()) {
			case VectorType::FLAT_VECTOR:
			case VectorType::CONSTANT_VECTOR:
			case VectorType::DICTIONARY_VECTOR:
				break;
			default:
				vec.Flatten();
				break;
			}
		}
		VectorOperations::GenerateSequence(row_ids, data.size(), row_start, 1);
	}
};

} // namespace duckdb
