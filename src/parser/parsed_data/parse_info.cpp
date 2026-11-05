#include "duckdb/parser/parsed_data/parse_info.hpp"
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
	case CatalogType::ROLE_ENTRY:
		return "ROLE";
	case CatalogType::DATABASE_ENTRY:
		return "DATABASE";
	case CatalogType::FOREIGN_SERVER_ENTRY:
		return "SERVER";
	case CatalogType::TOKENIZER_ENTRY:
		return "TEXT SEARCH DICTIONARY";
	case CatalogType::SUBSCRIPTION_ENTRY:
		return "SUBSCRIPTION";
	default:
		throw InternalException("ParseInfo::TypeToString for CatalogType with type: %s not implemented",
		                        EnumUtil::ToString(type));
	}
}

string ParseInfo::RoutineKeyword(bool include_functions, bool include_procedures) {
	return !include_procedures ? "FUNCTION" : !include_functions ? "PROCEDURE" : "ROUTINE";
}

string ParseInfo::RoutineToString(const QualifiedName &name, bool has_func_args,
                                  const vector<LogicalType> &parameters) {
	auto result = name.ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	if (has_func_args) {
		result +=
		    "(" +
		    StringUtil::Join(parameters, ", ", [](const LogicalType &parameter) { return parameter.ToString(); }) + ")";
	}
	return result;
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

} // namespace duckdb
