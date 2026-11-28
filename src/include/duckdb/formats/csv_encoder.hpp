//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/csv_encoder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/read_within_page.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/formats/csv_simd.hpp"

namespace duckdb {

class CsvSpecials {
public:
	void Add(char byte);
	bool In(std::string_view field) const {
		if (field.size() - 1 < CSV_BLOCK_BYTES - 1 && ReadableWithinPage(field.data(), CSV_BLOCK_BYTES)) {
			return (CsvVector(field.data()).Lookup(low, high) & ((uint64_t(1) << field.size()) - 1)) != 0;
		}
		return Find(field, 0) < field.size();
	}
	idx_t Find(std::string_view field, idx_t from) const;
	bool Quote(std::string_view field, std::string_view null_str) const {
		return field == null_str || In(field);
	}

private:
	uint8_t low[16] = {};
	uint8_t high[16] = {};
	idx_t groups = 0;
};

idx_t CsvEscapes(std::string_view field, char quote, char escape);
idx_t CsvWriteQuoted(char *out, std::string_view field, char quote, char escape);
idx_t CsvQuoteInPlace(char *data, idx_t size, char quote, char escape);

struct CsvEncoding {
	char delimiter;
	char quote;
	char escape;
	const string &null_str;
	const vector<bool> &force_quote;
	const CsvSpecials &specials;
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
