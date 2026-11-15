#include "duckdb/execution/operator/helper/physical_reset.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/enums/set_scope.hpp"
#include "duckdb/execution/operator/helper/physical_set.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

static bool CanResetBelowGlobal(const ConfigurationOption &option) {
	if (option.default_value) {
		return option.scope != SettingScopeTarget::GLOBAL_ONLY;
	}
	return option.reset_local != nullptr;
}

static bool CanResetAt(const ExtensionOption &option, SetScope effective_scope) {
	return !(option.default_scope == SetScope::GLOBAL && effective_scope != SetScope::GLOBAL) &&
	       !(option.default_scope == SetScope::SESSION && effective_scope == SetScope::LOCAL);
}

static bool ResetToStartValue(ClientContext &context, const Identifier &name, SetScope effective_scope) {
	auto &start_values = effective_scope == SetScope::GLOBAL ? DBConfig::GetConfig(context).options.user_options
	                                                         : ClientConfig::GetConfig(context).session_defaults;
	auto entry = start_values.find(name);
	if (entry == start_values.end()) {
		return false;
	}
	PhysicalSet::SetVariable(context, name, effective_scope, entry->second, false);
	return true;
}

void PhysicalReset::ResetExtensionVariable(ClientContext &context, DBConfig &config, const Identifier &name,
                                           ExtensionOption &extension_option, SetScope effective_scope) {
	// Compatibility: mirror the checks from PhysicalSet::SetExtensionVariable.
	if (!CanResetAt(extension_option, effective_scope)) {
		throw InvalidInputException("parameter \"%s\" cannot be reset locally", extension_option.description);
	}
	if (effective_scope == SetScope::GLOBAL && !context.transaction.IsAutoCommit()) {
		throw InvalidInputException("parameter \"%s\" is global and cannot be reset inside a transaction",
		                            extension_option.description);
	}
	if (ResetToStartValue(context, name, effective_scope)) {
		return;
	}

	// RESET on an option the user never SET is a no-op — don't reapply the
	// default via set_function, which would needlessly trigger side-effect
	// callbacks (e.g. guards that reject any write).
	auto &client_config = ClientConfig::GetConfig(context);
	auto setting_index = extension_option.setting_index.GetIndex();
	bool is_user_set = effective_scope == SetScope::GLOBAL || client_config.user_settings.IsSet(setting_index);
	if (!is_user_set) {
		return;
	}

	// The rollback observer only needs to see real changes; gate it with
	// is_user_set so RESET ALL no-ops don't reach the sdb tracker.
	context.OnSettingChange(name.GetIdentifierName(), effective_scope,
	                        PhysicalSet::SessionValue(context, name, extension_option.setting_index));

	if (extension_option.reset_function) {
		extension_option.reset_function(context, effective_scope);
	} else if (extension_option.set_function) {
		extension_option.set_function(context, effective_scope, extension_option.default_value);
	}
	if (effective_scope == SetScope::GLOBAL) {
		config.ResetOption(extension_option);
	} else {
		client_config.user_settings.ClearSetting(setting_index);
	}
	if (extension_option.rebind) {
		PhysicalSet::Rebind(context, effective_scope);
	}
}

void PhysicalReset::ResetOption(ClientContext &context, DBConfig &config, const ConfigurationOption &option,
                                const char *name, SetScope variable_scope) {
	config.CheckLock(option.name);
	if (ResetToStartValue(context, Identifier(name), variable_scope)) {
		return;
	}

	if (option.default_value) {
		// RESET on an untouched generic option is a no-op — don't fire
		// set_callback with default_value (write-rejecting guards would throw).
		auto &client_config = ClientConfig::GetConfig(context);
		auto setting_index = option.setting_idx.GetIndex();
		bool is_user_set = variable_scope == SetScope::GLOBAL || client_config.user_settings.IsSet(setting_index);
		if (!is_user_set) {
			return;
		}
		// Gate the rollback observer on is_user_set so RESET ALL no-ops don't
		// reach the sdb tracker.
		context.OnSettingChange(name, variable_scope,
		                        PhysicalSet::SessionValue(context, Identifier(name), setting_index));
		if (option.set_callback) {
			SettingCallbackInfo info(context, variable_scope);
			info.is_reset = true;
			auto parameter_type = DBConfig::ParseLogicalType(option.parameter_type);
			Value reset_val = Value(option.default_value).CastAs(context, parameter_type);
			option.set_callback(info, reset_val);
		}
		if (variable_scope == SetScope::SESSION || variable_scope == SetScope::LOCAL) {
			client_config.user_settings.ClearSetting(setting_index);
		} else {
			config.ResetGenericOption(setting_index);
		}
		if (option.rebind) {
			PhysicalSet::Rebind(context, variable_scope);
		}
		return;
	}
	switch (variable_scope) {
	case SetScope::GLOBAL: {
		if (!option.set_global) {
			throw CatalogException("option \"%s\" cannot be reset globally", option.name);
		}
		context.OnSettingChange(name, variable_scope, Value());
		auto &db = DatabaseInstance::GetDatabase(context);
		config.ResetOption(&db, option);
		break;
	}
	case SetScope::LOCAL:
	case SetScope::SESSION: {
		if (!option.reset_local) {
			throw CatalogException("option \"%s\" cannot be reset locally", option.name);
		}
		if (option.get_local) {
			auto local_value = option.get_local(context);
			if (local_value.IsNull()) {
				return;
			}
			context.OnSettingChange(name, variable_scope, local_value);
			option.reset_local(context);
			break;
		}
		// Custom-impl reset: no is_user_set signal available. Wrap with
		// pre/post equality and fire the handler only if the reset actually
		// changed the value.
		Value pre_value;
		const bool had_pre = context.TryGetCurrentSetting(option.name, pre_value);
		option.reset_local(context);
		Value post_value;
		const bool had_post = context.TryGetCurrentSetting(option.name, post_value);
		const bool changed = had_pre != had_post || !Value::NotDistinctFrom(pre_value, post_value);
		if (changed) {
			context.OnSettingChange(name, variable_scope, pre_value);
		}
		break;
	}
	default:
		throw InternalException("Unsupported SetScope for variable");
	}
	if (option.rebind) {
		PhysicalSet::Rebind(context, variable_scope);
	}
}

SourceResultType PhysicalReset::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                OperatorSourceInput &input) const {
	if (scope == SetScope::LOCAL && context.client.transaction.IsAutoCommit()) {
		const char *msg = "RESET LOCAL can only be used in transaction blocks";
		if (!context.client.EmitWarning(msg)) {
			throw InvalidInputException(msg);
		}
	}
	if (scope == SetScope::VARIABLE) {
		// SQL user variables live in a separate namespace from configuration
		// options. PhysicalSetVariable does not notify OnSettingChange
		// either, so skip it here for symmetry — the hook is config-option
		// rollback infrastructure, not variable tracking.
		auto &client_config = ClientConfig::GetConfig(context.client);
		client_config.ResetUserVariable(name);
		return SourceResultType::FINISHED;
	}
	if (name.empty()) {
		ResetAll(context.client, scope, false);
		return SourceResultType::FINISHED;
	}
	ResetVariable(context.client, Identifier(name.ToStdString()), scope);
	return SourceResultType::FINISHED;
}

void PhysicalReset::ResetVariable(ClientContext &context, const Identifier &name, SetScope scope) {
	auto &config = DBConfig::GetConfig(context);

	// Single-target RESET: the handler call moved inside ResetOption /
	// ResetExtensionVariable so no-op RESETs don't reach sdb's tracker.
	auto option = DBConfig::GetOptionByName(name);
	if (option) {
		const auto variable_scope = PhysicalSet::GetSettingScope(*option, scope);
		Settings::CheckChange(context, name, option->access, variable_scope);
		ResetOption(context, config, *option, name.GetIdentifierName().c_str(), variable_scope);
		return;
	}
	ExtensionOption extension_option;
	if (!config.TryGetExtensionOption(name, extension_option)) {
		if (Settings::IsCustomName(name)) {
			PhysicalSet::SetCustomVariable(context, name, scope, Value(string()));
			return;
		}
		Catalog::AutoloadExtensionByConfigName(context, name);
		if (!config.TryGetExtensionOption(name, extension_option)) {
			throw InvalidInputException("Extension parameter %s was not found after autoloading", name);
		}
	}
	const auto variable_scope = PhysicalSet::GetSettingScope(extension_option, scope);
	Settings::CheckChange(context, name, extension_option.access, variable_scope);
	ResetExtensionVariable(context, config, name, extension_option, variable_scope);
}

void PhysicalReset::ResetAll(ClientContext &context, SetScope scope, bool discard) {
	// RESET ALL / RESET LOCAL ALL: reset every session-level option. Skip
	// GLOBAL-scoped settings — those live DB-wide and PG's RESET ALL does not
	// touch them.
	auto &config = DBConfig::GetConfig(context);
	auto options_count = DBConfig::GetOptionCount();
	for (idx_t i = 0; i < options_count; i++) {
		auto option = DBConfig::GetOptionByIndex(i);
		if (!option || option->no_reset_all || !CanResetBelowGlobal(*option)) {
			continue;
		}
		auto variable_scope = PhysicalSet::GetSettingScope(*option, scope);
		if (variable_scope == SetScope::GLOBAL) {
			continue;
		}
		ResetOption(context, config, *option, option->name, variable_scope);
	}
	for (auto &ext : config.GetExtensionSettings()) {
		auto variable_scope = PhysicalSet::GetSettingScope(ext.second, scope);
		if ((ext.second.no_reset_all && !(discard && ext.second.reset_function)) ||
		    variable_scope == SetScope::GLOBAL || !CanResetAt(ext.second, variable_scope)) {
			continue;
		}
		ResetExtensionVariable(context, config, ext.first, ext.second, variable_scope);
	}
	if (scope == SetScope::GLOBAL) {
		return;
	}
	for (auto &entry : ClientConfig::GetConfig(context).custom_settings) {
		PhysicalSet::SetCustomVariable(context, Identifier(entry.first), scope, Value(string()));
	}
}

} // namespace duckdb
