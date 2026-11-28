#include "duckdb/parser/parsed_data/create_foreign_data_wrapper_info.hpp"

namespace duckdb {

CreateForeignDataWrapperInfo::CreateForeignDataWrapperInfo()
    : CreateInfo(CatalogType::FOREIGN_DATA_WRAPPER_ENTRY, Identifier::InvalidSchema()) {
}

unique_ptr<CreateInfo> CreateForeignDataWrapperInfo::Copy() const {
	auto result = make_uniq<CreateForeignDataWrapperInfo>();
	CopyProperties(*result);
	return std::move(result);
}

} // namespace duckdb
