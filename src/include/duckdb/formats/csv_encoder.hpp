//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/csv_encoder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace duckdb {

struct CsvEncoding {
	char delimiter;
	char quote;
	char escape;
	const string &null_str;
	const vector<bool> &force_quote;
	const vector<bool> &requires_quotes;
	const string &newline;
	bool newline_before;
};

struct CsvEncodeScratch {
	vector<uint32_t> sizes;
	vector<uint8_t> quoting;
	vector<idx_t> cursors;
};

void CsvEncodeChunk(const CsvEncoding &encoding, DataChunk &input, MemoryStream &out, bool &written_anything,
                    CsvEncodeScratch &scratch);
void CsvEncodeHeader(const CsvEncoding &encoding, const vector<Identifier> &names, MemoryStream &out);

} // namespace duckdb
