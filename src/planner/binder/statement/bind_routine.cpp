#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/permissions.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/parse_info.hpp"
#include "duckdb/planner/binder.hpp"

#include <absl/algorithm/container.h>

namespace duckdb {

BoundRoutine Binder::BindRoutine(const RoutineReference &routine, AclMode use, OnEntryNotFound if_not_found,
                                 bool named_by_statement) {
	for (auto &type : routine.func_parameters) {
		BindLogicalType(type);
	}
	auto &name = routine.name.Name().GetIdentifierName();
	BoundRoutine result;
	for (auto type : {CatalogType::MACRO_ENTRY, CatalogType::TABLE_MACRO_ENTRY, CatalogType::PRAGMA_FUNCTION_ENTRY}) {
		if (routine.type == CatalogType::MACRO_ENTRY || type == routine.type) {
			result.entry = entry_retriever.GetEntry(EntryLookupInfo(type, routine.name), OnEntryNotFound::RETURN_NULL);
		}
		if (result.entry) {
			break;
		}
	}
	optional_ptr<MacroCatalogEntry> macro;
	if (result.entry &&
	    (result.entry->type == CatalogType::MACRO_ENTRY || result.entry->type == CatalogType::TABLE_MACRO_ENTRY)) {
		macro = result.entry->Cast<MacroCatalogEntry>();
	}
	if (macro && !macro->internal && !routine.every_overload) {
		if (routine.has_func_args) {
			auto overload = absl::c_find_if(macro->macros, [&](const unique_ptr<MacroFunction> &candidate) {
				return candidate->HasParameterTypes(routine.func_parameters);
			});
			if (overload != macro->macros.end()) {
				result.overload = overload->get();
			}
		} else if (macro->macros.size() > 1) {
			const auto kind =
			    StringUtil::Lower(ParseInfo::RoutineKeyword(routine.include_functions, routine.include_procedures));
			auto extra_info = Exception::InitializeExtraInfo("AMBIGUOUS_FUNCTION", optional_idx());
			extra_info["hint"] = StringUtil::Format("Specify the argument list to select the %s unambiguously.", kind);
			throw CatalogException(extra_info, StringUtil::Format("%s name \"%s\" is not unique", kind, name));
		} else {
			result.overload = macro->macros[0].get();
		}
		if (!result.overload) {
			result.entry = nullptr;
		}
	}
	if (!result.entry) {
		if (if_not_found == OnEntryNotFound::RETURN_NULL) {
			return result;
		}
		const auto noun = routine.include_functions ? "function" : "procedure";
		auto extra_info = Exception::InitializeExtraInfo("MISSING_ENTRY", optional_idx());
		extra_info["type"] = CatalogTypeToString(CatalogType::MACRO_ENTRY);
		throw CatalogException(extra_info,
		                       routine.has_func_args
		                           ? StringUtil::Format("%s %s(%s) does not exist", noun, name,
		                                                MacroFunction::ParameterTypesToString(routine.func_parameters))
		                           : StringUtil::Format("could not find a %s named \"%s\"", noun, name));
	}
	const bool procedure = result.overload ? result.overload->is_procedure : macro && macro->is_procedure;
	const bool kinds_differ = result.entry->ParentCatalog().Compatibility() == SqlCompatibility::POSTGRES;
	if (kinds_differ && (procedure ? !routine.include_procedures : !routine.include_functions)) {
		throw CatalogException(
		    Exception::InitializeExtraInfo("WRONG_OBJECT_TYPE", optional_idx()),
		    StringUtil::Format("%s(%s) is not a %s", result.entry->name.GetIdentifierName(),
		                       MacroFunction::ParameterTypesToString(result.overload ? result.overload->ParameterTypes()
		                                                                             : routine.func_parameters),
		                       procedure ? "function" : "procedure"));
	}
	const StatementNaming naming {!routine.include_procedures  ? "function"
	                              : !routine.include_functions ? "procedure"
	                                                           : "routine",
	                              routine.name};
	Require(context, CheckAs(), *result.entry, use, result.overload.get(), named_by_statement ? &naming : nullptr);
	return result;
}

} // namespace duckdb
