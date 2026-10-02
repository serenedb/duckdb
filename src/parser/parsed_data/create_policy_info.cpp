#include "duckdb/parser/parsed_data/create_policy_info.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

CreatePolicyInfo::CreatePolicyInfo() : CreateInfo(CatalogType::POLICY_ENTRY, Identifier::InvalidSchema()) {
}

unique_ptr<CreateInfo> CreatePolicyInfo::Copy() const {
	auto result = make_uniq<CreatePolicyInfo>();
	CopyProperties(*result);
	result->SetPolicyName(GetPolicyName());
	result->base_table = base_table ? unique_ptr_cast<TableRef, BaseTableRef>(base_table->Copy()) : nullptr;
	result->relation_oid = relation_oid;
	result->permissive = permissive;
	result->command = command;
	result->role_names = role_names;
	result->roles = roles;
	result->using_expr = using_expr ? using_expr->Copy() : nullptr;
	result->check_expr = check_expr ? check_expr->Copy() : nullptr;
	return std::move(result);
}

string CreatePolicyInfo::RolesToString(const vector<Identifier> &role_names) {
	vector<string> names;
	for (auto &role_name : role_names) {
		auto &name = role_name.GetIdentifierName();
		const bool role_spec = StringUtil::CIEquals(name, "PUBLIC") || StringUtil::CIEquals(name, "CURRENT_USER") ||
		                       StringUtil::CIEquals(name, "CURRENT_ROLE") || StringUtil::CIEquals(name, "SESSION_USER");
		names.push_back(role_spec ? StringUtil::Upper(name) : SQLIdentifier::ToString(name));
	}
	return StringUtil::Join(names, ", ");
}

string CreatePolicyInfo::ToString() const {
	string result = "CREATE POLICY " + SQLIdentifier(GetPolicyName()) + " ON ";
	result += base_table ? base_table->ToString() : string();
	if (!permissive) {
		result += " AS RESTRICTIVE";
	}
	if (command != PolicyCommand::ALL) {
		result += " FOR " + EnumUtil::ToString(command);
	}
	if (!role_names.empty()) {
		result += " TO " + RolesToString(role_names);
	}
	if (using_expr) {
		result += " USING (" + using_expr->ToString() + ")";
	}
	if (check_expr) {
		result += " WITH CHECK (" + check_expr->ToString() + ")";
	}
	return result + ";";
}

} // namespace duckdb
