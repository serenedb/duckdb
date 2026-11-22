#include "duckdb/parser/parsed_data/create_db_role_setting_info.hpp"

namespace duckdb {

CreateDbRoleSettingInfo::CreateDbRoleSettingInfo()
    : CreateInfo(CatalogType::DB_ROLE_SETTING_ENTRY, Identifier::InvalidSchema()) {
}

Identifier CreateDbRoleSettingInfo::NameOf(idx_t database, idx_t role) {
	return Identifier(std::to_string(database) + "." + std::to_string(role));
}

unique_ptr<CreateInfo> CreateDbRoleSettingInfo::Copy() const {
	auto result = make_uniq<CreateDbRoleSettingInfo>();
	CopyProperties(*result);
	result->database = database;
	result->role = role;
	result->config = config;
	return std::move(result);
}

} // namespace duckdb
