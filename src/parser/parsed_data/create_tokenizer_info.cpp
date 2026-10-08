#include "duckdb/parser/parsed_data/create_tokenizer_info.hpp"

namespace duckdb {

CreateTokenizerInfo::CreateTokenizerInfo() : CreateInfo(CatalogType::TOKENIZER_ENTRY) {
}

unique_ptr<CreateInfo> CreateTokenizerInfo::Copy() const {
	auto result = make_uniq<CreateTokenizerInfo>();
	CopyProperties(*result);
	result->features = features;
	result->config = config;
	result->definition = definition;
	return std::move(result);
}

string CreateTokenizerInfo::ToString() const {
	string result = "CREATE TEXT SEARCH DICTIONARY ";
	if (on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		result += "IF NOT EXISTS ";
	}
	result += QualifiedNameToString() + " AS " + definition;
	string flags;
	for (auto &feature : TOKENIZER_FEATURES) {
		if (features & feature.bit) {
			flags += flags.empty() ? "" : ", ";
			flags += feature.name;
		}
	}
	if (!flags.empty()) {
		result += " WITH (" + flags + ")";
	}
	return result + ";";
}

} // namespace duckdb
