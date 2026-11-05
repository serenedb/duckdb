#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/parser/parsed_data/extra_drop_info.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"

namespace duckdb {

DropInfo::DropInfo() : ParseInfo(TYPE), cascade(false) {
}

DropInfo::DropInfo(const DropInfo &info)
    : ParseInfo(info.info_type), type(info.type), if_not_found(info.if_not_found), cascade(info.cascade),
      allow_drop_internal(info.allow_drop_internal), func_parameters(info.func_parameters),
      has_func_args(info.has_func_args), is_procedure(info.is_procedure), is_macro(info.is_macro),
      extra_drop_info(info.extra_drop_info ? info.extra_drop_info->Copy() : nullptr),
      qualified_name(info.qualified_name) {
	for (auto &target : info.additional_drops) {
		additional_drops.push_back(target->Copy());
	}
}

unique_ptr<DropInfo> DropInfo::Copy() const {
	return make_uniq<DropInfo>(*this);
}

string DropInfo::ToString() const {
	string result = "";
	if (type == CatalogType::PREPARED_STATEMENT) {
		if (extra_drop_info) {
			result += "DISCARD ";
			result += EnumUtil::ToString(extra_drop_info->Cast<ExtraDropDiscardInfo>().target);
		} else if (GetQualifiedName().Name().empty()) {
			result += "DEALLOCATE ALL";
		} else {
			result += "DEALLOCATE PREPARE ";
			result += SQLIdentifier(GetQualifiedName().Name());
		}
	} else {
		result += "DROP";
		if (is_procedure) {
			result += " PROCEDURE";
		} else if (type == CatalogType::MACRO_ENTRY && !is_macro) {
			result += " FUNCTION";
		} else {
			result += " " + ParseInfo::TypeToString(type);
		}
		if (if_not_found == OnEntryNotFound::RETURN_NULL) {
			result += " IF EXISTS";
		}
		auto target = [&](const DropInfo &info) {
			result += ParseInfo::RoutineToString(info.qualified_name, info.has_func_args, info.func_parameters);
		};
		result += " ";
		target(*this);
		for (auto &other : additional_drops) {
			result += ", ";
			target(*other);
		}
		if (type == CatalogType::TRIGGER_ENTRY && extra_drop_info) {
			auto &trigger_info = extra_drop_info->Cast<ExtraDropTriggerInfo>();
			if (trigger_info.base_table) {
				result += " ON ";
				result += trigger_info.base_table->Cast<BaseTableRef>().ToString();
			}
		}
		if (type == CatalogType::SECRET_ENTRY && extra_drop_info) {
			// Preserve the `FROM <storage>` storage specifier so `DROP SECRET <name> FROM <storage>` round-trips.
			auto &secret_info = extra_drop_info->Cast<ExtraDropSecretInfo>();
			if (!secret_info.secret_storage.empty()) {
				result += " FROM " + SQLIdentifier(secret_info.secret_storage);
			}
		}
		if (cascade) {
			result += " CASCADE";
		}
	}
	result += ";";
	return result;
}

} // namespace duckdb
