#include "duckdb/main/settings.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

bool Settings::TryGetSettingInternal(const ClientContext &context, idx_t setting_index, Value &result) {
	return context.TryGetCurrentUserSetting(setting_index, result);
}

bool Settings::TryGetSettingInternal(const DBConfig &config, idx_t setting_index, Value &result) {
	auto lookup_result = config.TryGetCurrentUserSetting(setting_index, result);
	return lookup_result;
}

bool Settings::TryGetSettingInternal(const DatabaseInstance &db, idx_t setting_index, Value &result) {
	return TryGetSettingInternal(DBConfig::GetConfig(db), setting_index, result);
}

void Settings::SetSettingInternal(DatabaseInstance &db, idx_t setting_index, SetScope scope, Value target_value) {
	SetSettingInternal(DBConfig::GetConfig(db), setting_index, scope, std::move(target_value));
}

void Settings::SetSettingInternal(DBConfig &config, idx_t setting_index, SetScope scope, Value target_value) {
	if (scope != SetScope::GLOBAL) {
		throw InternalException(
		    "Attempting to set setting with index %d with non-global scope, but no ClientContext was provided",
		    setting_index);
	}
	config.SetOption(setting_index, std::move(target_value));
}

void Settings::SetSettingInternal(ClientContext &context, idx_t setting_index, SetScope scope, Value target_value) {
	if (scope == SetScope::GLOBAL) {
		SetSettingInternal(DBConfig::GetConfig(context), setting_index, scope, std::move(target_value));
		return;
	}
	auto &client_config = ClientConfig::GetConfig(context);
	client_config.user_settings.SetUserSetting(setting_index, std::move(target_value));
}

Value Settings::FormatDisplayValue(ClientContext &context, const Value &value) {
	if (!value.IsNull() && value.type().id() == LogicalTypeId::BOOLEAN) {
		return Value(BooleanValue::Get(value) ? "on" : "off");
	}
	return value.CastAs(context, LogicalType::VARCHAR);
}

void Settings::ResolveCurrentValues(ClientContext &context, vector<string> &config) {
	for (auto &entry : config) {
		if (entry.find('=') != string::npos) {
			continue;
		}
		Value value;
		if (!context.TryGetCurrentSetting(Identifier(entry), value)) {
			throw InvalidInputException("unrecognized configuration parameter \"%s\"", entry);
		}
		entry += "=" + FormatDisplayValue(context, value).ToString();
	}
}

bool Settings::IsValidCustomName(std::string_view name) {
	bool separated = false;
	bool part_start = true;
	for (const auto c : name) {
		if (c == '.') {
			if (part_start) {
				return false;
			}
			separated = true;
			part_start = true;
		} else if (StringUtil::CharacterIsAlpha(c) || c == '_' || static_cast<unsigned char>(c) >= 0x80) {
			part_start = false;
		} else if (part_start || (!StringUtil::CharacterIsDigit(c) && c != '$')) {
			return false;
		}
	}
	return separated && !part_start;
}

void Settings::CheckCustomName(const Identifier &name) {
	const auto &text = name.GetIdentifierName();
	if (text.find('.') != string::npos && !IsValidCustomName(text)) {
		throw Exception(
		    {{"error_subtype", "INVALID_NAME"},
		     {"detail", "Custom parameter names must be two or more simple identifiers separated by dots."}},
		    ExceptionType::INVALID_INPUT, StringUtil::Format("invalid configuration parameter name \"%s\"", text));
	}
}

bool Settings::IsCustomName(const Identifier &name) {
	CheckCustomName(name);
	return name.GetIdentifierName().find('.') != string::npos;
}

optional<SettingAccess> Settings::GetAccess(ClientContext &context, const Identifier &name) {
	if (auto option = DBConfig::GetOptionByName(name)) {
		return option->access;
	}
	ExtensionOption extension_option;
	if (DBConfig::GetConfig(context).TryGetExtensionOption(name, extension_option)) {
		return extension_option.access;
	}
	if (IsValidCustomName(name.GetIdentifierName())) {
		return SettingAccess();
	}
	return optional<SettingAccess>();
}

static AclMode HeldParameterPrivileges(ClientContext &context, const Identifier &name) {
	auto &config = DBConfig::GetConfig(context);
	if (config.cluster_catalog.empty()) {
		return AclMode::NoRights;
	}
	auto &cluster = Catalog::GetCatalog(context, config.cluster_catalog).Cast<DuckCatalog>();
	auto holder = cluster.GetOidIndex().GetVisible(config.bootstrap_role, cluster.GetCatalogTransaction(context).view);
	if (!holder) {
		return AclMode::NoRights;
	}
	return context.CurrentRole().HeldModes(holder->permissions.ParameterAcl(name.GetIdentifierName()));
}

void Settings::CheckChange(ClientContext &context, const Identifier &name, const SettingAccess &access,
                           SetScope scope) {
	if (access.context == SettingContext::INTERNAL) {
		throw Exception(unordered_map<string, string> {{"error_subtype", "CANT_CHANGE_RUNTIME_PARAM"}},
		                ExceptionType::SETTINGS,
		                StringUtil::Format("parameter \"%s\" cannot be changed", name.GetIdentifierName()));
	}
	if (context.CurrentRole().is_superuser) {
		return;
	}
	if (scope == SetScope::GLOBAL) {
		if ((HeldParameterPrivileges(context, name) & AclMode::AlterSystem) != AclMode::NoRights) {
			return;
		}
		throw PermissionException(
		    {{"detail", "Only roles with the SUPERUSER attribute may change the server-wide value of a parameter."}},
		    StringUtil::Format("permission denied to set parameter \"%s\"", name.GetIdentifierName()));
	}
	if (access.context == SettingContext::SUPERUSER &&
	    (HeldParameterPrivileges(context, name) & AclMode::Set) == AclMode::NoRights) {
		throw PermissionException(
		    {{"detail", "Only roles with the SUPERUSER attribute may change this parameter."}},
		    StringUtil::Format("permission denied to set parameter \"%s\"", name.GetIdentifierName()));
	}
}

bool Settings::CanRead(ClientContext &context, const SettingAccess &access) {
	return !access.superuser_read || context.CurrentRole().Has(RoleCapability::ReadAllSettings);
}

void Settings::CheckRead(ClientContext &context, const Identifier &name) {
	auto access = GetAccess(context, name);
	if (access && !CanRead(context, *access)) {
		throw PermissionException(
		    {{"detail", "Only roles with privileges of the \"pg_read_all_settings\" role may examine this parameter."}},
		    StringUtil::Format("permission denied to examine \"%s\"", name.GetIdentifierName()));
	}
}

bool Settings::IsVisible(ClientContext &context, const Identifier &name) {
	auto access = GetAccess(context, name);
	return !access || (!access->no_show_all && CanRead(context, *access));
}

} // namespace duckdb
