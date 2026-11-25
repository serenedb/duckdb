#include "duckdb/parser/parsed_data/parse_info.hpp"

#include <absl/algorithm/container.h>

#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

string ParseInfo::TypeToString(CatalogType type) {
	switch (type) {
	case CatalogType::TABLE_ENTRY:
		return "TABLE";
	case CatalogType::SCALAR_FUNCTION_ENTRY:
		return "FUNCTION";
	case CatalogType::INDEX_ENTRY:
		return "INDEX";
	case CatalogType::SCHEMA_ENTRY:
		return "SCHEMA";
	case CatalogType::TYPE_ENTRY:
		return "TYPE";
	case CatalogType::VIEW_ENTRY:
		return "VIEW";
	case CatalogType::SEQUENCE_ENTRY:
		return "SEQUENCE";
	case CatalogType::MACRO_ENTRY:
		return "MACRO";
	case CatalogType::TABLE_MACRO_ENTRY:
		return "MACRO TABLE";
	case CatalogType::SECRET_ENTRY:
		return "SECRET";
	case CatalogType::TRIGGER_ENTRY:
		return "TRIGGER";
	case CatalogType::JOB_ENTRY:
		return "JOB";
	case CatalogType::DATABASE_ENTRY:
		return "DATABASE";
	case CatalogType::ROLE_ENTRY:
		return "ROLE";
	case CatalogType::TOKENIZER_ENTRY:
		return "TEXT SEARCH DICTIONARY";
	case CatalogType::FOREIGN_SERVER_ENTRY:
		return "SERVER";
	case CatalogType::TABLESPACE_ENTRY:
		return "TABLESPACE";
	case CatalogType::SUBSCRIPTION_ENTRY:
		return "SUBSCRIPTION";
	case CatalogType::FOREIGN_DATA_WRAPPER_ENTRY:
		return "FOREIGN DATA WRAPPER";
	case CatalogType::PARAMETER_ENTRY:
		return "PARAMETER";
	case CatalogType::LANGUAGE_ENTRY:
		return "LANGUAGE";
	default:
		throw InternalException("ParseInfo::TypeToString for CatalogType with type: %s not implemented",
		                        EnumUtil::ToString(type));
	}
}

string ParseInfo::RoutineKeyword(bool include_functions, bool include_procedures) {
	return !include_procedures ? "FUNCTION" : !include_functions ? "PROCEDURE" : "ROUTINE";
}

string RoleSpec::ToString() const {
	if (type != RoleSpecType::NAME) {
		return EnumUtil::ToString(type);
	}
	if (IsPublic()) {
		return "PUBLIC";
	}
	auto &text = name.GetIdentifierName();
	const bool keyword = StringUtil::CIEquals(text, "public") || StringUtil::CIEquals(text, "current_user") ||
	                     StringUtil::CIEquals(text, "session_user") || StringUtil::CIEquals(text, "current_role");
	return keyword ? SQLQuotedIdentifier::ToString(name) : SQLIdentifier::ToString(name);
}

string RoleSpec::ToString(const vector<RoleSpec> &roles) {
	return StringUtil::Join(roles, ", ", [](const RoleSpec &role) { return role.ToString(); });
}

string RenderOptionList(const case_insensitive_map_t<unique_ptr<ParsedExpression>> &parsed_options,
                        const unordered_map<string, Value> &options) {
	if (parsed_options.empty() && options.empty()) {
		return string();
	}
	vector<string> stringified;
	for (auto &opt : parsed_options) {
		stringified.push_back(StringUtil::Format("%s %s", SQLIdentifier::ToString(opt.first), opt.second->ToString()));
	}
	for (auto &opt : options) {
		stringified.push_back(
		    StringUtil::Format("%s %s", SQLIdentifier::ToString(opt.first), opt.second.ToSQLString()));
	}
	return " (" + StringUtil::Join(stringified, ", ") + ")";
}

string RenderSubscriptionOptions(const case_insensitive_map_t<Value> &options) {
	vector<string> items;
	for (auto &option : options) {
		items.push_back(option.first + " = " + option.second.ToSQLString());
	}
	absl::c_sort(items);
	return "(" + StringUtil::Join(items, ", ") + ")";
}

} // namespace duckdb
