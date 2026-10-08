#include "duckdb/execution/operator/helper/physical_reset.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/enums/set_scope.hpp"
#include "duckdb/execution/operator/helper/physical_set.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

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

void PhysicalReset::ResetExtensionVariable(ExecutionContext &context, DBConfig &config, const Identifier &name,
                                           ExtensionOption &extension_option, SetScope effective_scope) const {
	// Compatibility: mirror the checks from PhysicalSet::SetExtensionVariable.
	if (!CanResetAt(extension_option, effective_scope)) {
		throw InvalidInputException("parameter \"%s\" cannot be reset locally", extension_option.description);
	}
	if (effective_scope == SetScope::GLOBAL && !context.client.transaction.IsAutoCommit()) {
		throw InvalidInputException("parameter \"%s\" is global and cannot be reset inside a transaction",
		                            extension_option.description);
	}

	// RESET on an option the user never SET is a no-op — don't reapply the
	// default via set_function, which would needlessly trigger side-effect
	// callbacks (e.g. guards that reject any write).
	auto &client_config = ClientConfig::GetConfig(context.client);
	auto setting_index = extension_option.setting_index.GetIndex();
	bool is_user_set = effective_scope == SetScope::GLOBAL || client_config.user_settings.IsSet(setting_index);
	if (!is_user_set) {
		return;
	}

	// The rollback observer only needs to see real changes; gate it with
	// is_user_set so RESET ALL no-ops don't reach the sdb tracker.
	if (context.client.setting_change_handler) {
		context.client.setting_change_handler(context.client, name.GetIdentifierName(), effective_scope, nullptr);
	}

	if (extension_option.reset_function) {
		extension_option.reset_function(context.client, effective_scope);
	} else if (extension_option.set_function) {
		extension_option.set_function(context.client, effective_scope, extension_option.default_value);
	}
	if (effective_scope == SetScope::GLOBAL) {
		config.ResetOption(extension_option);
	} else {
		client_config.user_settings.ClearSetting(setting_index);
	}
}

void PhysicalReset::ResetOption(ExecutionContext &context, DBConfig &config, const ConfigurationOption &option,
                                SetScope variable_scope) const {
	config.CheckLock(option.name);

	if (option.default_value) {
		// RESET on an untouched generic option is a no-op — don't fire
		// set_callback with default_value (write-rejecting guards would throw).
		auto &client_config = ClientConfig::GetConfig(context.client);
		auto setting_index = option.setting_idx.GetIndex();
		bool is_user_set = variable_scope == SetScope::GLOBAL || client_config.user_settings.IsSet(setting_index);
		if (!is_user_set) {
			return;
		}
		// Gate the rollback observer on is_user_set so RESET ALL no-ops don't
		// reach the sdb tracker.
		if (context.client.setting_change_handler) {
			context.client.setting_change_handler(context.client, option.name, variable_scope, nullptr);
		}
		if (option.set_callback) {
			SettingCallbackInfo info(context.client, variable_scope);
			info.is_reset = true;
			auto parameter_type = DBConfig::ParseLogicalType(option.parameter_type);
			Value reset_val = Value(option.default_value).CastAs(context.client, parameter_type);
			option.set_callback(info, reset_val);
		}
		if (variable_scope == SetScope::SESSION || variable_scope == SetScope::LOCAL) {
			client_config.user_settings.ClearSetting(setting_index);
		} else {
			config.ResetGenericOption(setting_index);
		}
		return;
	}
	switch (variable_scope) {
	case SetScope::GLOBAL: {
		if (!option.set_global) {
			throw CatalogException("option \"%s\" cannot be reset globally", option.name);
		}
		auto &db = DatabaseInstance::GetDatabase(context.client);
		config.ResetOption(&db, option);
		break;
	}
	case SetScope::LOCAL:
	case SetScope::SESSION: {
		if (!option.reset_local) {
			throw CatalogException("option \"%s\" cannot be reset locally", option.name);
		}
		// Custom-impl reset: no is_user_set signal available. Wrap with
		// pre/post equality and fire the handler only if the reset actually
		// changed the value.
		Value pre_value;
		const bool had_pre = context.client.TryGetCurrentSetting(option.name, pre_value);
		option.reset_local(context.client);
		Value post_value;
		const bool had_post = context.client.TryGetCurrentSetting(option.name, post_value);
		const bool changed = had_pre != had_post || !Value::NotDistinctFrom(pre_value, post_value);
		if (changed && context.client.setting_change_handler) {
			context.client.setting_change_handler(context.client, option.name, variable_scope, nullptr);
		}
		break;
	}
	default:
		throw InternalException("Unsupported SetScope for variable");
	}
}

SourceResultType PhysicalReset::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                OperatorSourceInput &input) const {
	// RESET LOCAL is only meaningful inside a transaction; reject outside.
	if (scope == SetScope::LOCAL && context.client.transaction.IsAutoCommit()) {
		throw InvalidInputException("RESET LOCAL can only be used in transaction blocks");
	}
	if (scope == SetScope::VARIABLE) {
		// SQL user variables live in a separate namespace from configuration
		// options. PhysicalSetVariable does not notify setting_change_handler
		// either, so skip it here for symmetry — the handler is config-option
		// rollback infrastructure, not variable tracking.
		auto &client_config = ClientConfig::GetConfig(context.client);
		client_config.ResetUserVariable(name);
		return SourceResultType::FINISHED;
	}
	auto &config = DBConfig::GetConfig(context.client);
	if (name.empty()) {
		ResetAll(context, config);
		return SourceResultType::FINISHED;
	}

	// Single-target RESET: the handler call moved inside ResetOption /
	// ResetExtensionVariable so no-op RESETs don't reach sdb's tracker.
	Identifier option_name(name.ToStdString());
	auto option = DBConfig::GetOptionByName(option_name);
	if (option) {
		ResetOption(context, config, *option, PhysicalSet::GetSettingScope(*option, scope));
		return SourceResultType::FINISHED;
	}
	ExtensionOption extension_option;
	if (!config.TryGetExtensionOption(option_name, extension_option)) {
		Catalog::AutoloadExtensionByConfigName(context.client, option_name);
		if (!config.TryGetExtensionOption(option_name, extension_option)) {
			throw InvalidInputException("Extension parameter %s was not found after autoloading", name);
		}
	}
	ResetExtensionVariable(context, config, option_name, extension_option,
	                       PhysicalSet::GetSettingScope(extension_option, scope));
	return SourceResultType::FINISHED;
}

void PhysicalReset::ResetAll(ExecutionContext &context, DBConfig &config) const {
	// RESET ALL / RESET LOCAL ALL: reset every session-level option. Skip
	// GLOBAL-scoped settings — those live DB-wide and PG's RESET ALL does not
	// touch them.
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
		ResetOption(context, config, *option, variable_scope);
	}
	for (auto &ext : config.GetExtensionSettings()) {
		auto variable_scope = PhysicalSet::GetSettingScope(ext.second, scope);
		if (ext.second.no_reset_all || variable_scope == SetScope::GLOBAL || !CanResetAt(ext.second, variable_scope)) {
			continue;
		}
		ResetExtensionVariable(context, config, ext.first, ext.second, variable_scope);
	}
}

} // namespace duckdb
