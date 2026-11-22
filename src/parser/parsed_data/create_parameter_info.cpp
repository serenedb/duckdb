#include "duckdb/parser/parsed_data/create_parameter_info.hpp"

namespace duckdb {

CreateParameterInfo::CreateParameterInfo() : CreateInfo(CatalogType::PARAMETER_ENTRY, Identifier::InvalidSchema()) {
}

unique_ptr<CreateInfo> CreateParameterInfo::Copy() const {
	auto result = make_uniq<CreateParameterInfo>();
	CopyProperties(*result);
	return std::move(result);
}

} // namespace duckdb
