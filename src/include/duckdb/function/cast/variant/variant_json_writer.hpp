//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/cast/variant/variant_json_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/json_document.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/variant.hpp"
#include "duckdb/common/types/variant_iterator.hpp"

namespace duckdb {

class VariantJSONWriter {
public:
	VariantJSONWriter();

	optional<string_t> Write(const UnifiedVariantVectorData &variant, idx_t row, uint32_t values_idx, string &error);
	optional<string_t> WriteText(const VariantNode &variant, string &error);

private:
	optional<string_t> Result(string &error) const;

private:
	JSONStringWriter writer;
};

} // namespace duckdb
