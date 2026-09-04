#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_macro_catalog_entry.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/function/scalar_macro_function.hpp"
#include "duckdb/parser/parsed_data/alter_scalar_function_info.hpp"

namespace duckdb {

MacroCatalogEntry::MacroCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateMacroInfo &info)
    : FunctionEntry(
          (info.macros[0]->type == MacroType::SCALAR_MACRO ? CatalogType::MACRO_ENTRY : CatalogType::TABLE_MACRO_ENTRY),
          catalog, schema, info),
      macros(std::move(info.macros)), is_procedure(info.is_procedure) {
	this->temporary = info.temporary;
	this->internal = info.internal;
	this->extension_name = info.extension_name;
	this->dependencies = info.dependencies;
	this->comment = info.comment;
	this->tags = info.tags;
}

ScalarMacroCatalogEntry::ScalarMacroCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateMacroInfo &info)
    : MacroCatalogEntry(catalog, schema, info) {
}

unique_ptr<CatalogEntry> ScalarMacroCatalogEntry::Copy(ClientContext &context) const {
	auto info_copy = GetInfo();
	auto &cast_info = info_copy->Cast<CreateMacroInfo>();
	auto result = make_uniq<ScalarMacroCatalogEntry>(catalog, Schema(), cast_info);
	return std::move(result);
}

TableMacroCatalogEntry::TableMacroCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateMacroInfo &info)
    : MacroCatalogEntry(catalog, schema, info) {
}

unique_ptr<CatalogEntry> TableMacroCatalogEntry::Copy(ClientContext &context) const {
	auto info_copy = GetInfo();
	auto &cast_info = info_copy->Cast<CreateMacroInfo>();
	auto result = make_uniq<TableMacroCatalogEntry>(catalog, Schema(), cast_info);
	return std::move(result);
}

unique_ptr<CreateInfo> MacroCatalogEntry::GetInfo() const {
	auto info = make_uniq<CreateMacroInfo>(type);
	info->SetQualifiedName(QualifiedName(catalog.GetName(), ParentSchema().name, name));
	for (auto &function : macros) {
		info->macros.push_back(function->Copy());
	}
	info->is_procedure = is_procedure;
	info->extension_name = extension_name;
	info->dependencies = dependencies;
	info->comment = comment;
	info->tags = tags;
	info->oid = oid;
	info->parent_oid = ParentSchema().oid;
	return std::move(info);
}

unique_ptr<CatalogEntry> MacroCatalogEntry::AlterEntry(ClientContext &context, AlterInfo &info) {
	// The parser cannot tell a scalar from a table macro, so the rename arrives typed scalar for both.
	if (info.type == AlterType::ALTER_SCALAR_FUNCTION) {
		auto &function_info = info.Cast<AlterScalarFunctionInfo>();
		if (function_info.alter_scalar_function_type == AlterScalarFunctionType::RENAME_SCALAR_FUNCTION) {
			auto &rename_info = function_info.Cast<RenameScalarFunctionInfo>();
			auto copied = CopyPreservingIdentity(context);
			copied->name = rename_info.new_name;
			return copied;
		}
	}
	throw CatalogException("Can only rename a function with ALTER statement");
}

string MacroCatalogEntry::ToSQL() const {
	auto create_info = GetInfo();
	return create_info->ToString();
}

//! Align the info's catalog type with the overloads it holds: MACRO_ENTRY iff every one is scalar. A
//! TableMacroFunction left in the scalar bucket (or the reverse) breaks Cast<>() at lookup time.
static void RetagMacroInfo(CreateMacroInfo &info) {
	if (info.macros.empty()) {
		return;
	}
	bool all_scalar = true;
	for (auto &macro : info.macros) {
		if (macro->type != MacroType::SCALAR_MACRO) {
			all_scalar = false;
			break;
		}
	}
	info.type = all_scalar ? CatalogType::MACRO_ENTRY : CatalogType::TABLE_MACRO_ENTRY;
}

optional_ptr<const MacroFunction> MacroCatalogEntry::FindOverload(const vector<LogicalType> &parameters) const {
	for (auto &macro : macros) {
		if (macro->types == parameters) {
			return macro.get();
		}
	}
	return nullptr;
}

unique_ptr<CreateMacroInfo> MacroCatalogEntry::WithoutOverload(const vector<LogicalType> &parameters) const {
	auto info = GetInfo();
	auto &macro_info = info->Cast<CreateMacroInfo>();
	for (idx_t i = 0; i < macro_info.macros.size(); i++) {
		if (macro_info.macros[i]->types == parameters) {
			macro_info.macros.erase_at(i);
			RetagMacroInfo(macro_info);
			return unique_ptr_cast<CreateInfo, CreateMacroInfo>(std::move(info));
		}
	}
	return nullptr;
}

unique_ptr<CreateMacroInfo> MacroCatalogEntry::WithoutKind(bool procedures) const {
	auto info = GetInfo();
	auto &macro_info = info->Cast<CreateMacroInfo>();
	auto removed =
	    std::remove_if(macro_info.macros.begin(), macro_info.macros.end(),
	                   [&](const unique_ptr<MacroFunction> &macro) { return macro->is_procedure == procedures; });
	if (removed == macro_info.macros.end()) {
		return nullptr;
	}
	macro_info.macros.erase(removed, macro_info.macros.end());
	RetagMacroInfo(macro_info);
	return unique_ptr_cast<CreateInfo, CreateMacroInfo>(std::move(info));
}

unique_ptr<CreateMacroInfo> MacroCatalogEntry::MergedWith(const CreateMacroInfo &declared,
                                                          bool replace_matching) const {
	auto info = GetInfo();
	auto &merged = info->Cast<CreateMacroInfo>();
	for (auto &declared_macro : declared.macros) {
		bool found = false;
		for (auto &existing_macro : merged.macros) {
			if (existing_macro->types != declared_macro->types) {
				continue;
			}
			if (!replace_matching) {
				return nullptr;
			}
			existing_macro = declared_macro->Copy();
			found = true;
			break;
		}
		if (!found) {
			merged.macros.push_back(declared_macro->Copy());
		}
	}
	// Top-level dependencies track the merged overload set, not the superseded version they were copied from.
	merged.dependencies = MacroFunction::UnionDependencies(merged.macros);
	return unique_ptr_cast<CreateInfo, CreateMacroInfo>(std::move(info));
}

} // namespace duckdb
