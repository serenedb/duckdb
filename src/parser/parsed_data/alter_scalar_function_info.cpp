#include "duckdb/parser/parsed_data/alter_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// AlterScalarFunctionInfo
//===--------------------------------------------------------------------===//
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

//===--------------------------------------------------------------------===//
// RenameScalarFunctionInfo
//===--------------------------------------------------------------------===//
RenameScalarFunctionInfo::RenameScalarFunctionInfo()
    : AlterScalarFunctionInfo(AlterScalarFunctionType::RENAME_SCALAR_FUNCTION, AlterEntryData()) {
}

RenameScalarFunctionInfo::RenameScalarFunctionInfo(const AlterEntryData &data, Identifier new_name_p)
    : AlterScalarFunctionInfo(AlterScalarFunctionType::RENAME_SCALAR_FUNCTION, data), new_name(std::move(new_name_p)) {
}

RenameScalarFunctionInfo::~RenameScalarFunctionInfo() {
}

unique_ptr<AlterInfo> RenameScalarFunctionInfo::Copy() const {
	return make_uniq_base<AlterInfo, RenameScalarFunctionInfo>(GetAlterEntryData(), new_name);
}

string RenameScalarFunctionInfo::ToString() const {
	auto &qualified_name = GetQualifiedName();
	string result = "ALTER FUNCTION ";
	if (!qualified_name.Schema().empty()) {
		result += KeywordHelper::WriteOptionallyQuoted(qualified_name.Schema().GetIdentifierName()) + ".";
	}
	result += KeywordHelper::WriteOptionallyQuoted(qualified_name.Name().GetIdentifierName());
	result += " RENAME TO ";
	result += KeywordHelper::WriteOptionallyQuoted(new_name.GetIdentifierName());
	result += ";";
	return result;
}

} // namespace duckdb
