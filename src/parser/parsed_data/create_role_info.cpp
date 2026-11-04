#include "duckdb/parser/parsed_data/create_role_info.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
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

void AppendRoleOptions(vector<string> &result, RoleOption set, RoleOption clear) {
	static constexpr std::pair<RoleOption, const char *> KEYWORDS[] = {
	    {RoleOption::Superuser, "SUPERUSER"},   {RoleOption::Inherit, "INHERIT"},
	    {RoleOption::CreateRole, "CREATEROLE"}, {RoleOption::CreateDb, "CREATEDB"},
	    {RoleOption::Login, "LOGIN"},           {RoleOption::Replication, "REPLICATION"},
	    {RoleOption::BypassRls, "BYPASSRLS"},
	};
	for (auto &[option, keyword] : KEYWORDS) {
		if (HasOption(set, option)) {
			result.emplace_back(keyword);
		} else if (HasOption(clear, option)) {
			result.push_back(string("NO") + keyword);
		}
	}
}

string RoleValidUntilToString(int64_t valid_until) {
	const timestamp_t timestamp(valid_until);
	auto text = Timestamp::ToString(timestamp);
	return SQLString::ToString(Value::IsFinite(timestamp) ? text + "+00" : text);
}

static string RoleNames(const vector<Identifier> &names) {
	return StringUtil::Join(names, ", ", [](const Identifier &name) { return SQLIdentifier::ToString(name); });
}

string CreateRoleInfo::ToString() const {
	vector<string> clauses;
	AppendRoleOptions(clauses, options & ~RoleOption::Inherit, ~options & RoleOption::Inherit);
	if (conn_limit != -1) {
		clauses.push_back("CONNECTION LIMIT " + std::to_string(conn_limit));
	}
	if (valid_until != 0) {
		clauses.push_back("VALID UNTIL " + RoleValidUntilToString(valid_until));
	}
	if (!password.empty()) {
		clauses.push_back("PASSWORD " + SQLString::ToString(password));
	}
	if (!in_roles.empty()) {
		clauses.push_back("IN ROLE " + RoleNames(in_roles));
	}
	if (!role_members.empty()) {
		clauses.push_back("ROLE " + RoleNames(role_members));
	}
	if (!admin_members.empty()) {
		clauses.push_back("ADMIN " + RoleNames(admin_members));
	}
	auto result = "CREATE ROLE " + SQLIdentifier::ToString(GetQualifiedName().Name());
	if (!clauses.empty()) {
		result += " WITH " + StringUtil::Join(clauses, " ");
	}
	return result + ";";
}

} // namespace duckdb
