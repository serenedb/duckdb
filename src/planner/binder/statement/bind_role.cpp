#include "duckdb/catalog/catalog_entry/role_entry.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parsed_data/create_database_info.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

[[noreturn]] static void DenyRoleAction(const string &verb, const string &detail) {
	throw PermissionException({{"detail", detail}}, StringUtil::Format("permission denied to %s role", verb));
}

static void RequireAttributes(const RoleAccess &caller, const string &verb, const string &action, RoleOption options) {
	for (auto &[option, attribute] : {std::pair<RoleOption, const char *> {RoleOption::CreateDb, "CREATEDB"},
	                                  std::pair<RoleOption, const char *> {RoleOption::Replication, "REPLICATION"},
	                                  std::pair<RoleOption, const char *> {RoleOption::BypassRls, "BYPASSRLS"}}) {
		if (HasOption(options, option) && !caller.Has(option)) {
			DenyRoleAction(verb, StringUtil::Format("Only roles with the %s attribute may %s%s attribute.", attribute,
			                                        action, attribute));
		}
	}
}

static void RequireNotSuperuser(const RoleEntry &role, const string &verb) {
	if (role.IsSuperuser()) {
		DenyRoleAction(verb,
		               StringUtil::Format(
		                   "Only roles with the SUPERUSER attribute may %s roles with the SUPERUSER attribute.", verb));
	}
}

[[noreturn]] static void DenyRoleAdmin(const RoleEntry &role, const string &verb) {
	DenyRoleAction(
	    verb, StringUtil::Format("Only roles with the CREATEROLE attribute and the ADMIN option on role \"%s\" may "
	                             "%s this role.",
	                             role.name.GetIdentifierName(), verb));
}

static bool IsAdminOf(ClientContext &context, const RoleAccess &caller, idx_t role) {
	return context.security && context.security->MembershipsOf(caller.role).IsAdminOf(role);
}

static bool AdministersRole(ClientContext &context, const RoleAccess &caller, const RoleEntry &role) {
	return caller.Has(RoleOption::CreateRole) && IsAdminOf(context, caller, role.oid);
}

static void RequireRoleAdmin(ClientContext &context, const RoleAccess &caller, const RoleEntry &role,
                             const string &verb) {
	RequireNotSuperuser(role, verb);
	if (!AdministersRole(context, caller, role)) {
		DenyRoleAdmin(role, verb);
	}
}

static SettingAccess AccessOf(ClientContext &context, std::string_view key) {
	auto access = Settings::GetAccess(context, Identifier(string(key)));
	return access ? *access : SettingAccess();
}

static void CheckConfigNames(const vector<string> &set_config, const vector<string> &reset_config) {
	for (auto &entry : set_config) {
		Settings::CheckCustomName(Identifier(string(Settings::ConfigKey(entry))));
	}
	for (auto &key : reset_config) {
		Settings::CheckCustomName(Identifier(key));
	}
}

static void CheckConfigChange(ClientContext &context, const vector<string> &set_config, vector<string> &reset_config,
                              bool &reset_all_config, const vector<string> &current) {
	for (auto &entry : set_config) {
		const auto key = Settings::ConfigKey(entry);
		Settings::CheckChange(context, Identifier(string(key)), AccessOf(context, key), SetScope::SESSION);
	}
	for (auto &key : reset_config) {
		Settings::CheckChange(context, Identifier(key), AccessOf(context, key), SetScope::SESSION);
	}
	if (!reset_all_config) {
		return;
	}
	reset_all_config = false;
	for (auto &entry : current) {
		const auto key = Settings::ConfigKey(entry);
		if (AccessOf(context, key).context == SettingContext::USER) {
			reset_config.emplace_back(key);
		}
	}
}

static optional_ptr<RoleEntry> FindRole(Binder &binder, const string &name) {
	auto entry = binder.ClusterEntry(CatalogType::ROLE_ENTRY, Identifier(name));
	return entry ? &entry->Cast<RoleEntry>() : nullptr;
}

static void CheckDatabaseOwnerMembership(Binder &binder, const string &member, const string &granted) {
	const auto database_owner = DBConfig::GetConfig(binder.context).database_owner_role;
	if (database_owner == 0) {
		return;
	}
	if (auto role = FindRole(binder, granted); role && role->oid == database_owner) {
		throw Exception({{"error_subtype", "INVALID_GRANT_OPERATION"}}, ExceptionType::INVALID_INPUT,
		                StringUtil::Format("role \"%s\" cannot have explicit members", role->name.GetIdentifierName()));
	}
	if (auto role = FindRole(binder, member); role && role->oid == database_owner) {
		throw Exception(
		    {{"error_subtype", "INVALID_GRANT_OPERATION"}}, ExceptionType::INVALID_INPUT,
		    StringUtil::Format("role \"%s\" cannot be a member of any role", role->name.GetIdentifierName()));
	}
}

static void RequireGrantableRole(Binder &binder, const string &name, bool revoke) {
	auto role = FindRole(binder, name);
	if (!role) {
		return;
	}
	const string verb = revoke ? "revoke" : "grant";
	auto &role_name = role->name.GetIdentifierName();
	if (role->IsSuperuser()) {
		throw PermissionException(
		    {{"detail", StringUtil::Format("Only roles with the SUPERUSER attribute may %s roles with the SUPERUSER "
		                                   "attribute.",
		                                   verb)}},
		    StringUtil::Format("permission denied to %s role \"%s\"", verb, role_name));
	}
	if (!IsAdminOf(binder.context, binder.CheckAs(), role->oid)) {
		throw PermissionException(
		    {{"detail", StringUtil::Format("Only roles with the ADMIN option on role \"%s\" may %s this role.",
		                                   role_name, verb)}},
		    StringUtil::Format("permission denied to %s role \"%s\"", verb, role_name));
	}
}

void Binder::CheckCreateRole(const CreateRoleInfo &info) {
	auto &name = info.GetQualifiedName().Name().GetIdentifierName();
	for (auto &granted : info.in_roles) {
		CheckDatabaseOwnerMembership(*this, name, granted.GetIdentifierName());
	}
	for (auto &member : info.role_members) {
		CheckDatabaseOwnerMembership(*this, member.GetIdentifierName(), name);
	}
	for (auto &member : info.admin_members) {
		CheckDatabaseOwnerMembership(*this, member.GetIdentifierName(), name);
	}
	auto &caller = CheckAs();
	if (caller.is_superuser) {
		return;
	}
	if (!caller.Has(RoleOption::CreateRole)) {
		DenyRoleAction("create", "Only roles with the CREATEROLE attribute may create roles.");
	}
	if (HasOption(info.options, RoleOption::Superuser)) {
		DenyRoleAction("create",
		               "Only roles with the SUPERUSER attribute may create roles with the SUPERUSER attribute.");
	}
	RequireAttributes(caller, "create", "create roles with the ", info.options);
	for (auto &name : info.in_roles) {
		RequireGrantableRole(*this, name.GetIdentifierName(), false);
	}
}

void Binder::CheckAlterRole(AlterRoleInfo &info) {
	auto &caller = CheckAs();
	if (caller.IsSystem()) {
		return;
	}
	CheckConfigNames(info.set_config, info.reset_config);
	if (info.all_roles) {
		if (!caller.is_superuser) {
			throw PermissionException(
			    {{"detail", "Only roles with the SUPERUSER attribute may alter settings globally."}},
			    "permission denied to alter setting");
		}
		info.SetQualifiedName(Identifier(), Identifier(),
		                      Identifier(RoleName(DBConfig::GetConfig(context).bootstrap_role)));
		return;
	}
	auto role = FindRole(*this, info.GetQualifiedName().Name().GetIdentifierName());
	if (!role) {
		return;
	}
	if (info.grant_role.empty() && role->IsPredefined()) {
		throw Exception({{"error_subtype", "RESERVED_NAME"}, {"detail", "Cannot alter reserved roles."}},
		                ExceptionType::INVALID_INPUT,
		                StringUtil::Format("role name \"%s\" is reserved", role->name.GetIdentifierName()));
	}
	if (!info.grant_role.empty() && !info.revoke) {
		CheckDatabaseOwnerMembership(*this, role->name.GetIdentifierName(), info.grant_role);
	}
	if (caller.is_superuser) {
		return;
	}
	if (!info.grant_role.empty()) {
		RequireGrantableRole(*this, info.grant_role, info.revoke);
		return;
	}
	if (!info.new_name.empty()) {
		RequireRoleAdmin(context, caller, *role, "rename");
		return;
	}
	const auto changed = info.set_options | info.clear_options;
	const bool attributes = changed != RoleOption::None || info.set_conn_limit || info.set_valid_until;
	const bool other = role->oid != caller.role;
	RequireNotSuperuser(*role, "alter");
	if (HasOption(changed, RoleOption::Superuser)) {
		DenyRoleAction("alter", "Only roles with the SUPERUSER attribute may change the SUPERUSER attribute.");
	}
	if (AdministersRole(context, caller, *role)) {
		RequireAttributes(caller, "alter", "change the ", changed);
	} else if (attributes) {
		DenyRoleAdmin(*role, "alter");
	} else if (other && (info.set_password || info.null_password)) {
		DenyRoleAction("alter", "To change another role's password, the current user must have the CREATEROLE "
		                        "attribute and the ADMIN option on the role.");
	} else if (other) {
		DenyRoleAdmin(*role, "alter");
	}
	CheckConfigChange(context, info.set_config, info.reset_config, info.reset_all_config,
	                  role->GetInfo()->Cast<CreateRoleInfo>().config);
}

void Binder::CheckDropRole(const DropInfo &info) {
	auto &caller = CheckAs();
	if (caller.IsSystem()) {
		return;
	}
	const bool enforce = !caller.is_superuser;
	if (enforce && !caller.Has(RoleOption::CreateRole)) {
		DenyRoleAction(
		    "drop",
		    "Only roles with the CREATEROLE attribute and the ADMIN option on the target roles may drop roles.");
	}
	auto role = FindRole(*this, info.GetQualifiedName().Name().GetIdentifierName());
	if (!role) {
		return;
	}
	if (role->IsPredefined()) {
		throw DependencyException("cannot drop role %s because it is required by the database system",
		                          role->name.GetIdentifierName());
	}
	if (enforce) {
		RequireRoleAdmin(context, caller, *role, "drop");
	}
}

void Binder::CheckSharedObjectComment(AlterInfo &info) {
	auto &caller = CheckAs();
	if (caller.IsSystem()) {
		return;
	}
	auto &name = info.GetQualifiedName().Name();
	if (info.GetCatalogType() == CatalogType::DATABASE_ENTRY) {
		auto database = ClusterEntry(CatalogType::DATABASE_ENTRY, name);
		if (!database) {
			context.WarnUser(StringUtil::Format("database \"%s\" does not exist", name.GetIdentifierName()));
			info.if_not_found = OnEntryNotFound::RETURN_NULL;
			return;
		}
		if (!caller.is_superuser && !caller.MemberOf(database->permissions.owner)) {
			throw PermissionException("must be owner of database %s", name.GetIdentifierName());
		}
		return;
	}
	auto role = FindRole(*this, name.GetIdentifierName());
	if (!role) {
		throw CatalogException(unordered_map<string, string> {{"type", "Role"}},
		                       StringUtil::Format("role \"%s\" does not exist", name.GetIdentifierName()));
	}
	if (caller.is_superuser) {
		return;
	}
	string detail;
	if (role->IsSuperuser()) {
		detail = "The current user must have the SUPERUSER attribute.";
	} else if (!caller.Has(RoleOption::CreateRole)) {
		detail = "The current user must have the CREATEROLE attribute.";
	} else if (!IsAdminOf(context, caller, role->oid)) {
		detail = StringUtil::Format("The current user must have the ADMIN option on role \"%s\".",
		                            role->name.GetIdentifierName());
	} else {
		return;
	}
	throw PermissionException({{"detail", detail}}, "permission denied");
}

void Binder::CheckAlterDatabaseSettings(AlterDatabaseSettingsInfo &info) {
	auto &caller = CheckAs();
	if (caller.IsSystem()) {
		return;
	}
	CheckConfigNames(info.set_config, info.reset_config);
	auto &name = info.GetQualifiedName().Name();
	auto database = ClusterEntry(CatalogType::DATABASE_ENTRY, name);
	if (!database) {
		throw CatalogException(unordered_map<string, string> {{"type", "Database"}},
		                       StringUtil::Format("database \"%s\" does not exist", name.GetIdentifierName()));
	}
	if (caller.is_superuser) {
		return;
	}
	if (info.role_id == 0) {
		if (!caller.MemberOf(database->permissions.owner)) {
			throw PermissionException("must be owner of database %s", name.GetIdentifierName());
		}
	} else if (info.role_id != caller.role) {
		if (auto role = FindRole(*this, RoleName(info.role_id))) {
			RequireRoleAdmin(context, caller, *role, "alter");
		}
	}
	vector<string> current;
	for (auto &setting : database->GetInfo()->Cast<CreateDatabaseInfo>().settings) {
		if (setting.role == info.role_id) {
			current = setting.config;
		}
	}
	CheckConfigChange(context, info.set_config, info.reset_config, info.reset_all_config, current);
}

} // namespace duckdb
