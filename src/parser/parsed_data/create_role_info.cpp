#include "duckdb/parser/parsed_data/create_role_info.hpp"

#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

CreateRoleInfo::CreateRoleInfo() : CreateInfo(CatalogType::ROLE_ENTRY, Identifier::InvalidSchema()) {
}

unique_ptr<CreateInfo> CreateRoleInfo::Copy() const {
	auto result = make_uniq<CreateRoleInfo>();
	CopyProperties(*result);
	result->options = options;
	result->conn_limit = conn_limit;
	result->valid_until = valid_until;
	result->password = password;
	result->member_of = member_of;
	result->config = config;
	result->in_roles = in_roles;
	result->role_members = role_members;
	result->admin_members = admin_members;
	return std::move(result);
}

string CreateRoleInfo::ToString() const {
	return "CREATE ROLE " + KeywordHelper::WriteOptionallyQuoted(GetQualifiedName().Name().GetIdentifierName()) + ";";
}

} // namespace duckdb
