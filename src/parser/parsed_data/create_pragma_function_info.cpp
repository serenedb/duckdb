#include "duckdb/parser/parsed_data/create_pragma_function_info.hpp"

namespace duckdb {

CreatePragmaFunctionInfo::CreatePragmaFunctionInfo(PragmaFunction function)
    : CreateFunctionInfo(CatalogType::PRAGMA_FUNCTION_ENTRY), functions(function.name) {
	SetFunctionName(function.name);
	function.GetSignature().Verify();
	required_capability = function.required_capability;
	functions.AddFunction(std::move(function));
	internal = true;
}
CreatePragmaFunctionInfo::CreatePragmaFunctionInfo(PragmaFunctionSet functions_p)
    : CreateFunctionInfo(CatalogType::PRAGMA_FUNCTION_ENTRY), functions(std::move(functions_p)) {
	SetFunctionName(functions.name);
	for (auto &func : functions.functions) {
		func->GetSignature().Verify();
		required_capability |= func->required_capability;
	}
	internal = true;
}
CreatePragmaFunctionInfo::CreatePragmaFunctionInfo(Identifier name, PragmaFunctionSet functions_p)
    : CreateFunctionInfo(CatalogType::PRAGMA_FUNCTION_ENTRY), functions(std::move(functions_p)) {
	this->SetFunctionName(std::move(name));
	for (auto &func : functions.functions) {
		required_capability |= func->required_capability;
	}
	internal = true;
}

unique_ptr<CreateInfo> CreatePragmaFunctionInfo::Copy() const {
	auto result = make_uniq<CreatePragmaFunctionInfo>(functions.name, functions);
	CopyFunctionProperties(*result);
	return std::move(result);
}

} // namespace duckdb
