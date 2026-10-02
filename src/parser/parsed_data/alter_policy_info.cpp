#include "duckdb/parser/parsed_data/alter_policy_info.hpp"

#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parsed_data/create_policy_info.hpp"

namespace duckdb {

AlterPolicyInfo::AlterPolicyInfo() : AlterInfo(AlterType::ALTER_POLICY), alter_policy_type(AlterPolicyType::RENAME) {
}

AlterPolicyInfo::AlterPolicyInfo(AlterPolicyType alter_policy_type_p, AlterEntryData data,
                                 unique_ptr<BaseTableRef> base_table_p)
    : AlterInfo(AlterType::ALTER_POLICY, std::move(data.qualified_name), data.if_not_found),
      alter_policy_type(alter_policy_type_p), base_table(std::move(base_table_p)) {
}

AlterPolicyInfo::~AlterPolicyInfo() {
}

CatalogType AlterPolicyInfo::GetCatalogType() const {
	return CatalogType::POLICY_ENTRY;
}

unique_ptr<AlterInfo> AlterPolicyInfo::Copy() const {
	auto result =
	    make_uniq<AlterPolicyInfo>(alter_policy_type, GetAlterEntryData(),
	                               base_table ? unique_ptr_cast<TableRef, BaseTableRef>(base_table->Copy()) : nullptr);
	result->new_name = new_name;
	result->role_names = role_names;
	result->roles = roles;
	result->using_expr = using_expr ? using_expr->Copy() : nullptr;
	result->check_expr = check_expr ? check_expr->Copy() : nullptr;
	return std::move(result);
}

string AlterPolicyInfo::ToString() const {
	string result = "ALTER POLICY " + SQLIdentifier(GetQualifiedName().Name()) + " ON ";
	result += base_table ? base_table->ToString() : string();
	if (alter_policy_type == AlterPolicyType::RENAME) {
		return result + " RENAME TO " + SQLIdentifier(new_name) + ";";
	}
	if (!role_names.empty()) {
		result += " TO " + CreatePolicyInfo::RolesToString(role_names);
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
