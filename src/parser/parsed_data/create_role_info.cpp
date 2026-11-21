#include "duckdb/parser/parsed_data/create_role_info.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/sql_identifier.hpp"

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
	auto result =
	    "CREATE ROLE " + SQLIdentifier(GetQualifiedName().Name()) + " WITH" + RoleOptionsToSQL(options, ~options);
	if (conn_limit != -1) {
		result += " CONNECTION LIMIT " + std::to_string(conn_limit);
	}
	if (valid_until != 0) {
		result += " VALID UNTIL " + ValidUntilToSQL(valid_until);
	}
	if (!password.empty()) {
		result += " PASSWORD " + SQLString::ToString(password);
	}
	auto members = [&](const char *clause, const vector<Identifier> &roles) {
		if (!roles.empty()) {
			result += clause + StringUtil::Join(roles, ", ",
			                                    [](const Identifier &role) { return SQLIdentifier::ToString(role); });
		}
	};
	members(" IN ROLE ", in_roles);
	members(" ROLE ", role_members);
	members(" ADMIN ", admin_members);
	return result + ";";
}

} // namespace duckdb
