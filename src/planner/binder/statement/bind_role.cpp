#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/role_entry.hpp"
#include "duckdb/catalog/permissions.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_database_info.hpp"
#include "duckdb/parser/parsed_data/create_role_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

static SettingAccess AccessOf(ClientContext &context, const Identifier &name) {
	auto access = Settings::GetAccess(context, name);
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
		const Identifier name(string(Settings::ConfigKey(entry)));
		Settings::CheckChange(context, name, AccessOf(context, name), SetScope::SESSION);
	}
	for (auto &key : reset_config) {
		const Identifier name(key);
		Settings::CheckChange(context, name, AccessOf(context, name), SetScope::SESSION);
	}
	if (!reset_all_config) {
		return;
	}
	reset_all_config = false;
	for (auto &entry : current) {
		const auto key = Settings::ConfigKey(entry);
		const Identifier name {string(key)};
		if (Settings::MayChange(context, name, AccessOf(context, name), SetScope::SESSION)) {
			reset_config.emplace_back(key);
		}
	}
}

static void CheckMembershipAuthorization(ClientContext &context, const RoleAccess &caller, const RoleEntry &granted,
                                         bool revoke) {
	auto &name = granted.name.GetIdentifierName();
	if (!revoke && granted.oid == DBConfig::GetConfig(context).database_owner_role) {
		throw NotImplementedException("role \"%s\" cannot have explicit members", name);
	}
	if (caller.is_superuser) {
		return;
	}
	const string verb = revoke ? "revoke" : "grant";
	if (granted.IsSuperuser()) {
		throw PermissionException(
		    {{"detail",
		      StringUtil::Format("Only roles with the SUPERUSER attribute may %s roles with the SUPERUSER attribute.",
		                         verb)}},
		    StringUtil::Format("permission denied to %s role \"%s\"", verb, name));
	}
	if (!IsAdminOf(context, caller, granted.oid)) {
		throw PermissionException(
		    {{"detail",
		      StringUtil::Format("Only roles with the ADMIN option on role \"%s\" may %s this role.", name, verb)}},
		    StringUtil::Format("permission denied to %s role \"%s\"", verb, name));
	}
}

static void ResolveMembershipGrantor(ClientContext &context, const RoleAccess &caller, const RoleEntry &granted,
                                     AlterRoleInfo &info) {
	const auto bootstrap = DBConfig::GetConfig(context).bootstrap_role;
	const auto best_admin = [&](idx_t member) {
		return context.security ? context.security->BestAdmin(member, granted.oid) : optional_idx();
	};
	if (info.granted_by.empty()) {
		if (caller.is_superuser) {
			info.grantor_id = bootstrap;
			return;
		}
		auto grantor = best_admin(caller.role);
		if (!grantor.IsValid()) {
			throw InvalidInputException("no possible grantors");
		}
		info.grantor_id = grantor.GetIndex();
		return;
	}
	auto &grantor = *RoleSpecEntry(context, info.granted_by, OnEntryNotFound::THROW_EXCEPTION);
	auto &grantor_name = grantor.name.GetIdentifierName();
	if (!caller.is_superuser && caller.role != grantor.oid && !caller.Uses(grantor.oid)) {
		if (info.revoke) {
			throw PermissionException(
			    {{"detail", StringUtil::Format("Only roles with privileges of role \"%s\" may revoke privileges "
			                                   "granted by this role.",
			                                   grantor_name)}},
			    StringUtil::Format("permission denied to revoke privileges granted by role \"%s\"", grantor_name));
		}
		throw PermissionException(
		    {{"detail",
		      StringUtil::Format("Only roles with privileges of role \"%s\" may grant privileges as this role.",
		                         grantor_name)}},
		    StringUtil::Format("permission denied to grant privileges as role \"%s\"", grantor_name));
	}
	if (!info.revoke && grantor.oid != bootstrap) {
		auto admin = best_admin(grantor.oid);
		if (!admin.IsValid() || admin.GetIndex() != grantor.oid) {
			throw PermissionException(
			    {{"detail", StringUtil::Format("The grantor must have the ADMIN option on role \"%s\".",
			                                   granted.name.GetIdentifierName())}},
			    StringUtil::Format("permission denied to grant privileges as role \"%s\"", grantor_name));
		}
	}
	info.grantor_id = grantor.oid;
}

static void BindMembership(ClientContext &context, const RoleAccess &caller, const RoleEntry &member,
                           AlterRoleInfo &info) {
	auto granted = RoleSpecEntry(context, info.grant_role, OnEntryNotFound::RETURN_NULL);
	if (!granted) {
		return;
	}
	if (info.alter_group) {
		auto &name = granted->name.GetIdentifierName();
		if (granted->IsPredefined()) {
			throw Exception({{"error_subtype", "RESERVED_NAME"}, {"detail", "Cannot alter reserved roles."}},
			                ExceptionType::INVALID_INPUT, StringUtil::Format("role name \"%s\" is reserved", name));
		}
		if (!caller.is_superuser && granted->IsSuperuser()) {
			throw PermissionException(
			    {{"detail", "Only roles with the SUPERUSER attribute may alter roles with the SUPERUSER attribute."}},
			    "permission denied to alter role");
		}
		if (!caller.is_superuser && !IsAdminOf(context, caller, granted->oid)) {
			throw PermissionException(
			    {{"detail", StringUtil::Format(
			                    "Only roles with the ADMIN option on role \"%s\" may add or drop members.", name)}},
			    "permission denied to alter role");
		}
	} else {
		CheckMembershipAuthorization(context, caller, *granted, info.revoke);
	}
	ResolveMembershipGrantor(context, caller, *granted, info);
	if (!info.revoke && member.oid == DBConfig::GetConfig(context).database_owner_role) {
		throw NotImplementedException("role \"%s\" cannot be a member of any role", member.name.GetIdentifierName());
	}
}

static void CheckAlterRole(ClientContext &context, const RoleAccess &caller, const RoleEntry &role,
                           const AlterRoleInfo &info) {
	auto &name = role.name.GetIdentifierName();
	const bool rename = !info.new_name.empty();
	if (role.IsPredefined()) {
		throw Exception(
		    {{"error_subtype", "RESERVED_NAME"},
		     {"detail", rename ? "Role names starting with \"pg_\" are reserved." : "Cannot alter reserved roles."}},
		    ExceptionType::INVALID_INPUT, StringUtil::Format("role name \"%s\" is reserved", name));
	}
	if (caller.is_superuser) {
		return;
	}
	const bool admin = caller.Has(RoleOption::CreateRole) && IsAdminOf(context, caller, role.oid);
	if (rename && role.IsSuperuser()) {
		throw PermissionException(
		    {{"detail", "Only roles with the SUPERUSER attribute may rename roles with the SUPERUSER attribute."}},
		    "permission denied to rename role");
	}
	if (rename && !admin) {
		throw PermissionException(
		    {{"detail", StringUtil::Format("Only roles with the CREATEROLE attribute and the ADMIN option on role "
		                                   "\"%s\" may rename this role.",
		                                   name)}},
		    "permission denied to rename role");
	}
	if (rename) {
		return;
	}
	if (role.IsSuperuser()) {
		throw PermissionException(
		    {{"detail", "Only roles with the SUPERUSER attribute may alter roles with the SUPERUSER attribute."}},
		    "permission denied to alter role");
	}
	const auto changed = info.set_options | info.clear_options;
	if (HasOption(changed, RoleOption::Superuser)) {
		throw PermissionException(
		    {{"detail", "Only roles with the SUPERUSER attribute may change the SUPERUSER attribute."}},
		    "permission denied to alter role");
	}
	const bool other = role.oid != caller.role;
	if (!admin && (changed != RoleOption::None || info.set_conn_limit || info.set_valid_until)) {
		throw PermissionException(
		    {{"detail", StringUtil::Format("Only roles with the CREATEROLE attribute and the ADMIN option on role "
		                                   "\"%s\" may alter this role.",
		                                   name)}},
		    "permission denied to alter role");
	}
	if (!admin && other && (info.set_password || info.null_password)) {
		throw PermissionException(
		    {{"detail", "To change another role's password, the current user must have the CREATEROLE attribute and "
		                "the ADMIN option on the role."}},
		    "permission denied to alter role");
	}
	if (!admin && other) {
		throw PermissionException(
		    {{"detail", StringUtil::Format("Only roles with the CREATEROLE attribute and the ADMIN option on role "
		                                   "\"%s\" may alter this role.",
		                                   name)}},
		    "permission denied to alter role");
	}
	const auto attributes = changed & (RoleOption::CreateDb | RoleOption::Replication | RoleOption::BypassRls);
	for (auto &named : ROLE_OPTIONS) {
		if (HasOption(attributes, named.option) && !caller.Has(named.option)) {
			throw PermissionException(
			    {{"detail", StringUtil::Format("Only roles with the %s attribute may change the %s attribute.",
			                                   named.keyword, named.keyword)}},
			    "permission denied to alter role");
		}
	}
}

void Binder::BindCreateRole(CreateRoleInfo &info) {
	auto &caller = CheckAs();
	if (!caller.is_superuser && !caller.Has(RoleOption::CreateRole)) {
		throw PermissionException({{"detail", "Only roles with the CREATEROLE attribute may create roles."}},
		                          "permission denied to create role");
	}
	const auto attributes =
	    info.options & (RoleOption::Superuser | RoleOption::CreateDb | RoleOption::Replication | RoleOption::BypassRls);
	for (auto &named : ROLE_OPTIONS) {
		if (!caller.is_superuser && HasOption(attributes, named.option) && !caller.Has(named.option)) {
			throw PermissionException(
			    {{"detail",
			      StringUtil::Format("Only roles with the %s attribute may create roles with the %s attribute.",
			                         named.keyword, named.keyword)}},
			    "permission denied to create role");
		}
	}
	for (auto &name : info.in_roles) {
		if (auto granted = RoleSpecEntry(context, name.GetIdentifierName(), OnEntryNotFound::RETURN_NULL)) {
			CheckMembershipAuthorization(context, caller, *granted, false);
		}
	}
	const auto database_owner = DBConfig::GetConfig(context).database_owner_role;
	for (auto members : {&info.role_members, &info.admin_members}) {
		for (auto &name : *members) {
			auto member = RoleSpecEntry(context, name.GetIdentifierName(), OnEntryNotFound::RETURN_NULL);
			if (member && member->oid == database_owner) {
				throw NotImplementedException("role \"%s\" cannot be a member of any role",
				                              member->name.GetIdentifierName());
			}
		}
	}
	info.SetQualifiedName(ClusterName(context, info.GetQualifiedName()));
}

void Binder::BindAlterRole(AlterRoleInfo &info) {
	auto &caller = CheckAs();
	optional_ptr<RoleEntry> role;
	if (info.all_roles && !caller.is_superuser) {
		throw PermissionException({{"detail", "Only roles with the SUPERUSER attribute may alter settings globally."}},
		                          "permission denied to alter setting");
	}
	if (info.all_roles) {
		info.SetQualifiedName(Identifier(), Identifier(),
		                      Identifier(RoleName(context, DBConfig::GetConfig(context).bootstrap_role)));
	} else {
		role = RoleSpecEntry(context, info.GetQualifiedName().Name().GetIdentifierName(), OnEntryNotFound::RETURN_NULL);
	}
	if (role) {
		info.SetQualifiedName(Identifier(), Identifier(), role->name);
	}
	if (role && !caller.IsSystem() && !info.grant_role.empty()) {
		BindMembership(context, caller, *role, info);
	} else if (role && !caller.IsSystem()) {
		CheckAlterRole(context, caller, *role, info);
	}
	const bool settings = !info.set_config.empty() || !info.reset_config.empty() || info.reset_all_config;
	if (settings && !caller.IsSystem()) {
		CheckConfigNames(info.set_config, info.reset_config);
	}
	if (settings && role && !caller.IsSystem() && !caller.is_superuser) {
		CheckConfigChange(context, info.set_config, info.reset_config, info.reset_all_config,
		                  role->GetInfo()->Cast<CreateRoleInfo>().config);
	}
	Settings::ResolveCurrentValues(context, info.set_config);
	info.SetQualifiedName(ClusterName(context, info.GetQualifiedName()));
}

static void CheckDatabaseSettings(ClientContext &context, const RoleAccess &caller, optional_ptr<RoleEntry> role,
                                  AlterDatabaseSettingsInfo &info) {
	if (role && role->IsPredefined()) {
		throw Exception({{"error_subtype", "RESERVED_NAME"}, {"detail", "Cannot alter reserved roles."}},
		                ExceptionType::INVALID_INPUT,
		                StringUtil::Format("role name \"%s\" is reserved", role->name.GetIdentifierName()));
	}
	if (role && !caller.is_superuser && role->IsSuperuser()) {
		throw PermissionException(
		    {{"detail", "Only roles with the SUPERUSER attribute may alter roles with the SUPERUSER attribute."}},
		    "permission denied to alter role");
	}
	if (role && !caller.is_superuser && role->oid != caller.role &&
	    (!caller.Has(RoleOption::CreateRole) || !IsAdminOf(context, caller, role->oid))) {
		throw PermissionException(
		    {{"detail", StringUtil::Format("Only roles with the CREATEROLE attribute and the ADMIN option on role "
		                                   "\"%s\" may alter this role.",
		                                   role->name.GetIdentifierName())}},
		    "permission denied to alter role");
	}
	auto &name = info.GetQualifiedName().Name();
	auto &database = ClusterDatabase(context, name);
	if (!role && !caller.is_superuser && !caller.MemberOf(database.permissions.owner)) {
		throw PermissionException("must be owner of database %s", name.GetIdentifierName());
	}
	CheckConfigNames(info.set_config, info.reset_config);
	if (caller.is_superuser) {
		return;
	}
	vector<string> current;
	for (auto &setting : database.GetInfo()->Cast<CreateDatabaseInfo>().settings) {
		if (setting.role == info.role_id) {
			current = setting.config;
		}
	}
	CheckConfigChange(context, info.set_config, info.reset_config, info.reset_all_config, current);
}

void Binder::BindAlterDatabaseSettings(AlterDatabaseSettingsInfo &info) {
	auto &caller = CheckAs();
	optional_ptr<RoleEntry> role;
	if (!info.role.empty()) {
		role = RoleSpecEntry(context, info.role, OnEntryNotFound::THROW_EXCEPTION);
		info.role_id = role->oid;
	}
	if (!caller.IsSystem()) {
		CheckDatabaseSettings(context, caller, role, info);
	}
	Settings::ResolveCurrentValues(context, info.set_config);
	info.SetQualifiedName(ClusterName(context, info.GetQualifiedName()));
}

void Binder::BindDropRole(DropInfo &info) {
	auto &caller = CheckAs();
	if (!caller.IsSystem() && !caller.is_superuser && !caller.Has(RoleOption::CreateRole)) {
		throw PermissionException({{"detail", "Only roles with the CREATEROLE attribute and the ADMIN option on the "
		                                      "target roles may drop roles."}},
		                          "permission denied to drop role");
	}
	optional_ptr<RoleEntry> role;
	if (!caller.IsSystem()) {
		role = RoleSpecEntry(context, info.GetQualifiedName().Name().GetIdentifierName(), OnEntryNotFound::RETURN_NULL);
	}
	if (role && !caller.is_superuser && role->IsSuperuser()) {
		throw PermissionException(
		    {{"detail", "Only roles with the SUPERUSER attribute may drop roles with the SUPERUSER attribute."}},
		    "permission denied to drop role");
	}
	if (role && !caller.is_superuser && !IsAdminOf(context, caller, role->oid)) {
		throw PermissionException(
		    {{"detail", StringUtil::Format("Only roles with the CREATEROLE attribute and the ADMIN option on role "
		                                   "\"%s\" may drop this role.",
		                                   role->name.GetIdentifierName())}},
		    "permission denied to drop role");
	}
	if (role && role->IsPredefined()) {
		throw DependencyException("cannot drop role %s because it is required by the database system",
		                          role->name.GetIdentifierName());
	}
	info.SetQualifiedName(ClusterName(context, info.GetQualifiedName()));
}

} // namespace duckdb
