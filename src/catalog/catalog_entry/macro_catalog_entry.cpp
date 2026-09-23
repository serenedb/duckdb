#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_macro_catalog_entry.hpp"
#include "duckdb/function/scalar_macro_function.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"

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
	this->permissions = info.permissions;
}

ScalarMacroCatalogEntry::ScalarMacroCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateMacroInfo &info)
    : MacroCatalogEntry(catalog, schema, info) {
}

unique_ptr<CatalogEntry> ScalarMacroCatalogEntry::Copy(ClientContext &context) const {
	auto info_copy = GetInfo();
	auto &cast_info = info_copy->Cast<CreateMacroInfo>();
	auto result = make_uniq<ScalarMacroCatalogEntry>(catalog, ParentSchema(context), cast_info);
	return std::move(result);
}

TableMacroCatalogEntry::TableMacroCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateMacroInfo &info)
    : MacroCatalogEntry(catalog, schema, info) {
}

unique_ptr<CatalogEntry> TableMacroCatalogEntry::Copy(ClientContext &context) const {
	auto info_copy = GetInfo();
	auto &cast_info = info_copy->Cast<CreateMacroInfo>();
	auto result = make_uniq<TableMacroCatalogEntry>(catalog, ParentSchema(context), cast_info);
	return std::move(result);
}

unique_ptr<CreateInfo> MacroCatalogEntry::GetInfo() const {
	auto info = make_uniq<CreateMacroInfo>(type);
	info->SetQualifiedName(QualifiedName(catalog.GetName(), ParentSchemaName(), name));
	for (auto &function : macros) {
		info->macros.push_back(function->Copy());
	}
	info->is_procedure = is_procedure;
	info->extension_name = extension_name;
	info->dependencies = dependencies;
	info->comment = comment;
	info->tags = tags;
	info->permissions = permissions;
	return std::move(info);
}

unique_ptr<CatalogEntry> MacroCatalogEntry::AlterEntry(CatalogTransaction transaction, AlterInfo &info) {
	if (info.type != AlterType::REPLACE_DEFINITION) {
		return CatalogEntry::AlterEntry(transaction, info);
	}
	auto replaced = info.Cast<ReplaceDefinitionInfo>().definition->Copy();
	auto &replaced_macro = replaced->Cast<CreateMacroInfo>();
	if (replaced_macro.is_procedure != is_procedure) {
		throw BinderException("cannot change routine kind");
	}
	replaced_macro.comment = comment;
	replaced_macro.tags = tags;
	if (replaced_macro.type == CatalogType::MACRO_ENTRY) {
		return make_uniq<ScalarMacroCatalogEntry>(catalog, ParentSchema(transaction), replaced_macro);
	}
	return make_uniq<TableMacroCatalogEntry>(catalog, ParentSchema(transaction), replaced_macro);
}

string MacroCatalogEntry::ToSQL() const {
	auto create_info = GetInfo();
	return create_info->ToString();
}

} // namespace duckdb
