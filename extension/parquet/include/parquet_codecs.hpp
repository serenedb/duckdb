//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_codecs.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/formats/block_codecs.hpp"
#include "parquet_types.h"

namespace duckdb {

inline bool TryGetParquetBlockCodec(duckdb_parquet::CompressionCodec::type codec, BlockCodec &result) {
	switch (codec) {
	case duckdb_parquet::CompressionCodec::GZIP:
		result = BlockCodec::GZIP;
		return true;
	case duckdb_parquet::CompressionCodec::LZ4_RAW:
		result = BlockCodec::LZ4;
		return true;
	case duckdb_parquet::CompressionCodec::LZ4:
		result = BlockCodec::LZ4_HADOOP;
		return true;
	case duckdb_parquet::CompressionCodec::SNAPPY:
		result = BlockCodec::SNAPPY;
		return true;
	case duckdb_parquet::CompressionCodec::ZSTD:
		result = BlockCodec::ZSTD;
		return true;
	case duckdb_parquet::CompressionCodec::BROTLI:
		result = BlockCodec::BROTLI;
		return true;
	default:
		return false;
	}
}

} // namespace duckdb
