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
#include "duckdb/storage/storage_index.hpp"

namespace duckdb {

class LocalTableStorage;

//! One batch of rows handed to an external index, which tokenizes it on worker threads and so keeps
//! it past the loop that produced it.
struct ExternalIndexBatch {
	//! Scanned columns, in mapped_column_ids order.
	DataChunk data;
	//! The same columns in full table layout, referencing `data`.
	DataChunk view;
	Vector row_ids;
	//! Scanned strings point into the producer's segments; null when `data` owns everything it
	//! references, otherwise held so the memory outlives the last reader.
	shared_ptr<LocalTableStorage> source;

	ExternalIndexBatch() : row_ids(LogicalType::ROW_TYPE) {
	}

	//! Call once `data` has been scanned in `mapped_column_ids` order.
	void Finalize(const vector<LogicalType> &table_types, const vector<StorageIndex> &mapped_column_ids,
	              row_t row_start) {
		view.InitializeEmpty(table_types);
		for (idx_t i = 0; i < mapped_column_ids.size(); i++) {
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
			view.data[mapped_column_ids[i].GetPrimaryIndex()].Reference(vec);
		}
		view.SetCardinality(data.size());
		VectorOperations::GenerateSequence(row_ids, data.size(), row_start, 1);
	}

	//! Same, for producers whose `data` is already in full table layout (WAL replay): `view` just
	//! references it, so consumers always read `view` regardless of which producer built the batch.
	void FinalizeInTableLayout(row_t row_start) {
		view.InitializeEmpty(data.GetTypes());
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
			view.data[i].Reference(vec);
		}
		view.SetCardinality(data.size());
		VectorOperations::GenerateSequence(row_ids, data.size(), row_start, 1);
	}
};

} // namespace duckdb
