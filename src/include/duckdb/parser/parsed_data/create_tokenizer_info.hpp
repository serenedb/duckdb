//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/create_tokenizer_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/create_info.hpp"

#include <string_view>

namespace duckdb {

struct TokenizerFeature {
	uint64_t bit;
	std::string_view name;
};

inline constexpr TokenizerFeature TOKENIZER_FEATURES[] = {
    {1U << 0, "frequency"},
    {1U << 1, "position"},
    {1U << 2, "offset"},
    {1U << 4, "norm"},
};

struct CreateTokenizerInfo : public CreateInfo {
	CreateTokenizerInfo();

	uint64_t features = 0;
	string config;
	string definition;

public:
	DUCKDB_API void Serialize(Serializer &serializer) const override;
	DUCKDB_API static unique_ptr<CreateInfo> Deserialize(Deserializer &deserializer);

	unique_ptr<CreateInfo> Copy() const override;
	string ToString() const override;
};

} // namespace duckdb
