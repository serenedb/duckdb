#include "duckdb/parser/parsed_data/alter_scalar_function_info.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// AlterScalarFunctionInfo
//===--------------------------------------------------------------------===//
AlterScalarFunctionInfo::AlterScalarFunctionInfo(AlterScalarFunctionType type)
    : AlterInfo(AlterType::ALTER_SCALAR_FUNCTION), alter_scalar_function_type(type) {
}

AlterScalarFunctionInfo::AlterScalarFunctionInfo(AlterScalarFunctionType type, const AlterEntryData &data)
    : AlterInfo(AlterType::ALTER_SCALAR_FUNCTION, data.GetQualifiedName(), data.if_not_found),
      alter_scalar_function_type(type) {
}
AlterScalarFunctionInfo::~AlterScalarFunctionInfo() {
}

CatalogType AlterScalarFunctionInfo::GetCatalogType() const {
	return CatalogType::SCALAR_FUNCTION_ENTRY;
}

//===--------------------------------------------------------------------===//
// RenameScalarFunctionInfo
//===--------------------------------------------------------------------===//
unique_ptr<AlterInfo> RenameScalarFunctionInfo::Copy() const {
	auto result = make_uniq<RenameScalarFunctionInfo>(GetAlterEntryData(), new_name);
	result->include_functions = include_functions;
	result->include_procedures = include_procedures;
	result->func_parameters = func_parameters;
	result->has_func_args = has_func_args;
	result->overload_oid = overload_oid;
	return std::move(result);
}

string RenameScalarFunctionInfo::ToString() const {
	string result = "ALTER " + ParseInfo::RoutineKeyword(include_functions, include_procedures) + " ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += ParseInfo::RoutineToString(GetQualifiedName(), has_func_args, func_parameters);
	return result + " RENAME TO " + SQLIdentifier(new_name) + ";";
}

//===--------------------------------------------------------------------===//
// AlterFunctionAttributesInfo
//===--------------------------------------------------------------------===//
AlterFunctionAttributesInfo::AlterFunctionAttributesInfo()
    : AlterScalarFunctionInfo(AlterScalarFunctionType::SET_FUNCTION_ATTRIBUTES) {
}

AlterFunctionAttributesInfo::AlterFunctionAttributesInfo(const AlterEntryData &data)
    : AlterScalarFunctionInfo(AlterScalarFunctionType::SET_FUNCTION_ATTRIBUTES, data) {
}

unique_ptr<AlterInfo> AlterFunctionAttributesInfo::Copy() const {
	auto result = make_uniq<AlterFunctionAttributesInfo>(GetAlterEntryData());
	result->include_functions = include_functions;
	result->include_procedures = include_procedures;
	result->parameter_types = parameter_types;
	result->has_parameter_types = has_parameter_types;
	result->set_security = set_security;
	result->security_definer = security_definer;
	result->set_stability = set_stability;
	result->stability = stability;
	result->set_strict = set_strict;
	result->strict = strict;
	result->replace_config = replace_config;
	result->config = config;
	return std::move(result);
}

string AlterFunctionAttributesInfo::ToString() const {
	string result = "ALTER " + ParseInfo::RoutineKeyword(include_functions, include_procedures) + " " +
	                ParseInfo::RoutineToString(GetQualifiedName(), has_parameter_types, parameter_types);
	if (set_security) {
		result += security_definer ? " SECURITY DEFINER" : " SECURITY INVOKER";
	}
	if (set_stability) {
		result += stability == FunctionStability::CONSISTENT
		              ? " IMMUTABLE"
		              : (stability == FunctionStability::CONSISTENT_WITHIN_QUERY ? " STABLE" : " VOLATILE");
	}
	if (set_strict) {
		result += strict ? " STRICT" : " CALLED ON NULL INPUT";
	}
	if (replace_config && config.empty()) {
		result += " RESET ALL";
	}
	for (auto &entry : config) {
		auto separator = entry.find('=');
		result += separator == string::npos
		              ? " SET " + entry + " FROM CURRENT"
		              : " SET " + entry.substr(0, separator) + " TO " + entry.substr(separator + 1);
	}
	return result + ";";
}

//===--------------------------------------------------------------------===//
// AddScalarFunctionOverloadInfo
//===--------------------------------------------------------------------===//
AddScalarFunctionOverloadInfo::AddScalarFunctionOverloadInfo(const AlterEntryData &data,
                                                             unique_ptr<CreateScalarFunctionInfo> new_overloads_p)
    : AlterScalarFunctionInfo(AlterScalarFunctionType::ADD_FUNCTION_OVERLOADS, data),
      new_overloads(std::move(new_overloads_p)) {
	this->allow_internal = true;
}

AddScalarFunctionOverloadInfo::~AddScalarFunctionOverloadInfo() {
}

unique_ptr<AlterInfo> AddScalarFunctionOverloadInfo::Copy() const {
	return make_uniq_base<AlterInfo, AddScalarFunctionOverloadInfo>(
	    GetAlterEntryData(), unique_ptr_cast<CreateInfo, CreateScalarFunctionInfo>(new_overloads->Copy()));
}

string AddScalarFunctionOverloadInfo::ToString() const {
	throw NotImplementedException("NOT PARSABLE CURRENTLY");
}

} // namespace duckdb
