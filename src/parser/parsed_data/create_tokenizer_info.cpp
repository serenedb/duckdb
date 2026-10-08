#include "duckdb/parser/parsed_data/create_tokenizer_info.hpp"

namespace duckdb {

CreateTokenizerInfo::CreateTokenizerInfo() : CreateInfo(CatalogType::TOKENIZER_ENTRY) {
}

unique_ptr<CreateInfo> CreateTokenizerInfo::Copy() const {
	auto result = make_uniq<CreateTokenizerInfo>();
	CopyProperties(*result);
	result->features = features;
	result->config = config;
	return std::move(result);
}

string CreateTokenizerInfo::ToString() const {
	return "CREATE TEXT SEARCH DICTIONARY " + QualifiedNameToString() + ";";
}

} // namespace duckdb
