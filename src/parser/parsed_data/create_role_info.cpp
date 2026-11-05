#include "duckdb/parser/parsed_data/create_role_info.hpp"

#include "duckdb/common/string_util.hpp"
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
	result->all_roles_config = all_roles_config;
	result->in_roles = in_roles;
	result->role_members = role_members;
	result->admin_members = admin_members;
	return std::move(result);
}

string CreateRoleInfo::ToString() const {
	constexpr auto all_options = RoleOption::Superuser | RoleOption::Inherit | RoleOption::CreateRole |
	                             RoleOption::CreateDb | RoleOption::Login | RoleOption::Replication |
	                             RoleOption::BypassRls;
	auto result = "CREATE ROLE " + KeywordHelper::WriteOptionallyQuoted(GetQualifiedName().Name().GetIdentifierName()) +
	              " WITH" + RoleOptionsToSQL(options, all_options & ~options);
	if (conn_limit != -1) {
		result += " CONNECTION LIMIT " + std::to_string(conn_limit);
	}
	if (valid_until != 0) {
		result += " VALID UNTIL " + ValidUntilToSQL(valid_until);
	}
	if (!password.empty()) {
		result += " PASSWORD " + KeywordHelper::WriteQuoted(password, '\'');
	}
	auto members = [&](const char *clause, const vector<Identifier> &roles) {
		if (!roles.empty()) {
			result += clause + StringUtil::Join(roles, ", ", [](const Identifier &role) {
				          return KeywordHelper::WriteOptionallyQuoted(role.GetIdentifierName());
			          });
		}
	};
	members(" IN ROLE ", in_roles);
	members(" ROLE ", role_members);
	members(" ADMIN ", admin_members);
	return result + ";";
}

} // namespace duckdb
