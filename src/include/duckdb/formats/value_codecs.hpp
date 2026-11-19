//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/value_codecs.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

enum class ValueRendering : uint8_t { NATIVE, POSTGRES };

class ValueDecoder {
public:
	virtual ~ValueDecoder() = default;

	virtual bool Decode(Vector &text, Vector &out, idx_t count, string &error) = 0;
};

ValueRendering ValueRenderingFromString(const string &name, const string &option);
unique_ptr<ValueDecoder> MakeValueDecoder(ClientContext &context, const LogicalType &type, ValueRendering rendering);

} // namespace duckdb
