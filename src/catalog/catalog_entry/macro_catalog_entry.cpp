#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_macro_catalog_entry.hpp"
#include "duckdb/function/scalar_macro_function.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/exception/catalog_exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
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
	if (catalog.IsSystemCatalog()) {
		return;
	}
	auto &manager = catalog.GetDatabase().GetDatabaseManager();
	bool entry_oid_taken = false;
	for (auto &macro : macros) {
		if (macro->oid) {
			manager.ClaimOid(macro->oid);
		} else if (entry_oid_taken) {
			macro->oid = manager.NextOid();
		} else {
			entry_oid_taken = true;
		}
	}
}

idx_t MacroCatalogEntry::OverloadOid(const MacroFunction &overload) const {
	return overload.oid ? overload.oid : oid;
}

const Permissions &MacroCatalogEntry::OverloadPermissions(const MacroFunction &overload) const {
	return overload.permissions.owner ? overload.permissions : permissions;
}

const Value &MacroCatalogEntry::OverloadComment(const MacroFunction &overload) const {
	return overload.oid ? overload.comment : comment;
}

optional_ptr<MacroFunction> MacroCatalogEntry::FindOverload(idx_t overload_oid) const {
	for (auto &macro : macros) {
		if (OverloadOid(*macro) == overload_oid) {
			return macro.get();
		}
	}
	return nullptr;
}

vector<idx_t> MacroCatalogEntry::GetSubObjectOids() const {
	vector<idx_t> oids;
	for (auto &macro : macros) {
		if (macro->oid) {
			oids.push_back(macro->oid);
		}
	}
	return oids;
}

unique_ptr<MacroCatalogEntry> MacroCatalogEntry::Create(Catalog &catalog, SchemaCatalogEntry &schema,
                                                        CreateMacroInfo &info) {
	if (info.type == CatalogType::TABLE_MACRO_ENTRY) {
		return make_uniq<TableMacroCatalogEntry>(catalog, schema, info);
	}
	return make_uniq<ScalarMacroCatalogEntry>(catalog, schema, info);
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
	info->SetQualifiedName(GetQualifiedName(name));
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
	auto alter_overload = [&](idx_t overload_oid, auto &&alter) {
		auto altered = GetInfo();
		auto &altered_macros = altered->Cast<CreateMacroInfo>().macros;
		auto target =
		    std::find_if(altered_macros.begin(), altered_macros.end(),
		                 [&](const unique_ptr<MacroFunction> &macro) { return OverloadOid(*macro) == overload_oid; });
		if (target == altered_macros.end()) {
			throw CatalogException("function with OID %llu does not exist", overload_oid);
		}
		alter(**target);
		return Create(catalog, ParentSchema(transaction), altered->Cast<CreateMacroInfo>());
	};
	if (info.type == AlterType::ALTER_PERMISSIONS && info.Cast<AlterPermissionsInfo>().overload_oid) {
		auto &grant = info.Cast<AlterPermissionsInfo>();
		return alter_overload(grant.overload_oid, [&](MacroFunction &overload) {
			if (!overload.permissions.owner) {
				overload.permissions = permissions;
			}
			grant.ApplyTo(overload.permissions, type, nullptr);
		});
	}
	if (info.type == AlterType::SET_COMMENT && info.Cast<SetCommentInfo>().overload_oid &&
	    info.Cast<SetCommentInfo>().overload_oid != oid) {
		auto &comment_info = info.Cast<SetCommentInfo>();
		return alter_overload(comment_info.overload_oid,
		                      [&](MacroFunction &overload) { overload.comment = comment_info.comment_value; });
	}
	if (info.type != AlterType::REPLACE_DEFINITION) {
		return CatalogEntry::AlterEntry(transaction, info);
	}
	auto &definition = info.Cast<ReplaceDefinitionInfo>().definition->Cast<CreateMacroInfo>();
	definition.permissions = permissions;
	for (auto &function : definition.macros) {
		auto existing = std::find_if(macros.begin(), macros.end(), [&](const unique_ptr<MacroFunction> &macro) {
			return macro->HasParameterTypes(function->ParameterTypes());
		});
		if (existing != macros.end()) {
			function->oid = (*existing)->oid;
			function->permissions = (*existing)->permissions;
			function->comment = (*existing)->comment;
		} else if (!function->oid && !catalog.IsSystemCatalog()) {
			function->oid = catalog.GetDatabase().GetDatabaseManager().NextOid();
		}
	}
	auto replaced = definition.Copy();
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
	create_info->StripCatalogQualification();
	return create_info->ToString();
}

} // namespace duckdb
