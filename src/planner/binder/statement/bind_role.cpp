#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/database_entry.hpp"
#include "duckdb/catalog/catalog_entry/role_entry.hpp"
#include "duckdb/catalog/permissions.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_role_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

static SettingAccess AccessOf(ClientContext &context, const Identifier &name) {
	auto access = Settings::GetAccess(context, name);
	return access ? *access : SettingAccess();
}

static void CheckConfig(ClientContext &context, const vector<string> &set_config, const vector<string> &reset_config) {
	for (auto &entry : set_config) {
		Settings::CheckValue(context, Identifier(string(Settings::ConfigKey(entry))),
		                     entry.find('=') == string::npos ? Value() : Value(string(Settings::ConfigValue(entry))));
	}
	for (auto &key : reset_config) {
		Settings::CheckCustomName(Identifier(key));
	}
}

static void CheckConfigChange(ClientContext &context, const RoleAccess &caller, const vector<string> &set_config,
                              vector<string> &reset_config, bool &reset_all_config, const vector<string> &current) {
	for (auto &entry : set_config) {
		const Identifier name(string(Settings::ConfigKey(entry)));
		Settings::CheckChange(caller, name, AccessOf(context, name), SetScope::SESSION);
	}
	for (auto &key : reset_config) {
		const Identifier name(key);
		Settings::CheckChange(caller, name, AccessOf(context, name), SetScope::SESSION);
	}
	if (!reset_all_config) {
		return;
	}
	reset_all_config = false;
	for (auto &entry : current) {
		const auto key = Settings::ConfigKey(entry);
		const Identifier name {string(key)};
		if (Settings::MayChange(caller, AccessOf(context, name), SetScope::SESSION)) {
			reset_config.emplace_back(key);
		}
	}
}

enum class RoleChange : uint8_t { ALTER, RENAME, DROP, MEMBERS, GRANT, REVOKE };

static void RequireAdminOf(const RoleAccess &caller, const RoleEntry &role, RoleChange change) {
	if (caller.is_superuser) {
		return;
	}
	static constexpr const char *VERBS[] = {"alter", "rename", "drop", "alter", "grant", "revoke"};
	const auto verb = VERBS[static_cast<uint8_t>(change)];
	auto &name = role.name.GetIdentifierName();
	const bool grant = change == RoleChange::GRANT || change == RoleChange::REVOKE;
	const bool membership = grant || change == RoleChange::MEMBERS;
	const auto message = grant ? StringUtil::Format("permission denied to %s role \"%s\"", verb, name)
	                           : StringUtil::Format("permission denied to %s role", verb);
	if (role.IsSuperuser()) {
		throw PermissionException::WithDetail(
		    StringUtil::Format("Only roles with the SUPERUSER attribute may %s roles with the SUPERUSER attribute.",
		                       verb),
		    message);
	}
	if (caller.AdminOf(role.oid) && (membership || caller.Has(RoleOption::CreateRole))) {
		return;
	}
	string detail;
	if (change == RoleChange::MEMBERS) {
		detail = StringUtil::Format("Only roles with the ADMIN option on role \"%s\" may add or drop members.", name);
	} else if (membership) {
		detail = StringUtil::Format("Only roles with the ADMIN option on role \"%s\" may %s this role.", name, verb);
	} else {
		detail = StringUtil::Format(
		    "Only roles with the CREATEROLE attribute and the ADMIN option on role \"%s\" may %s this role.", name,
		    verb);
	}
	throw PermissionException::WithDetail(detail, message);
}

static void CheckMembershipAuthorization(ClientContext &context, const RoleAccess &caller, const RoleEntry &granted,
                                         bool revoke) {
	if (!revoke && granted.oid == DBConfig::GetConfig(context).database_owner_role) {
		throw NotImplementedException("role \"%s\" cannot have explicit members", granted.name.GetIdentifierName());
	}
	RequireAdminOf(caller, granted, revoke ? RoleChange::REVOKE : RoleChange::GRANT);
}

optional_ptr<RoleEntry> Binder::BindRole(const RoleSpec &role, OnEntryNotFound if_not_found) {
	if (role.type == RoleSpecType::NAME) {
		auto entry =
		    entry_retriever.GetEntry(EntryLookupInfo(CatalogType::ROLE_ENTRY, QualifiedName(role.name)), if_not_found);
		return entry ? &entry->Cast<RoleEntry>() : nullptr;
	}
	auto &system = Catalog::GetSystemCatalog(context);
	auto entry =
	    system.LookupOwner(system.GetCatalogTransaction(context),
	                       role.type == RoleSpecType::SESSION_USER ? context.session_role : context.current_role_id);
	if (entry && entry->type == CatalogType::ROLE_ENTRY) {
		return &entry->Cast<RoleEntry>();
	}
	if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
		throw CatalogException::MissingObject(CatalogType::ROLE_ENTRY, Identifier(role.ToString()));
	}
	return nullptr;
}

string Binder::RoleName(idx_t role) {
	auto &system = Catalog::GetSystemCatalog(context);
	auto entry = system.LookupOwner(system.GetCatalogTransaction(context), role);
	return entry && entry->type == CatalogType::ROLE_ENTRY ? entry->name.GetIdentifierName() : string();
}

static idx_t ResolveMembershipGrantor(Binder &binder, const RoleAccess &caller, const RoleEntry &granted,
                                      const AlterRoleInfo &info) {
	auto &context = binder.context;
	const auto bootstrap = DBConfig::GetConfig(context).bootstrap_role;
	const auto best_admin = [&](idx_t member) {
		return context.security ? context.security->BestAdmin(member, granted.oid) : optional_idx();
	};
	if (info.granted_by.empty()) {
		if (caller.is_superuser) {
			return bootstrap;
		}
		auto grantor = best_admin(caller.role);
		if (!grantor.IsValid()) {
			throw InvalidInputException("no possible grantors");
		}
		return grantor.GetIndex();
	}
	auto &grantor = *binder.BindRole(info.granted_by);
	auto &grantor_name = grantor.name.GetIdentifierName();
	if (!caller.MemberOf(grantor.oid)) {
		if (info.revoke) {
			throw PermissionException::WithDetail(
			    StringUtil::Format(
			        "Only roles with privileges of role \"%s\" may revoke privileges granted by this role.",
			        grantor_name),
			    "permission denied to revoke privileges granted by role \"%s\"", grantor_name);
		}
		throw PermissionException::WithDetail(
		    StringUtil::Format("Only roles with privileges of role \"%s\" may grant privileges as this role.",
		                       grantor_name),
		    "permission denied to grant privileges as role \"%s\"", grantor_name);
	}
	if (!info.revoke && grantor.oid != bootstrap) {
		auto admin = best_admin(grantor.oid);
		if (!admin.IsValid() || admin.GetIndex() != grantor.oid) {
			throw PermissionException::WithDetail(
			    StringUtil::Format("The grantor must have the ADMIN option on role \"%s\".",
			                       granted.name.GetIdentifierName()),
			    "permission denied to grant privileges as role \"%s\"", grantor_name);
		}
	}
	return grantor.oid;
}

static void BindMemberships(Binder &binder, const RoleAccess &caller, AlterRoleInfo &info) {
	auto &context = binder.context;
	vector<reference<RoleEntry>> members;
	for (auto &spec : info.members) {
		if (auto member = binder.BindRole(spec, OnEntryNotFound::RETURN_NULL)) {
			spec = RoleSpec(member->name);
			members.push_back(*member);
		}
	}
	for (auto &spec : info.granted_roles) {
		optional_ptr<RoleEntry> granted;
		if (!members.empty() && !caller.IsSystem()) {
			granted = binder.BindRole(spec, OnEntryNotFound::RETURN_NULL);
		}
		if (!granted) {
			info.grantor_ids.push_back(0);
			continue;
		}
		if (spec.type != RoleSpecType::NAME) {
			spec = RoleSpec(granted->name);
		}
		if (info.alter_group) {
			if (granted->IsPredefined()) {
				throw Exception({{"error_subtype", "RESERVED_NAME"}, {"detail", "Cannot alter reserved roles."}},
				                ExceptionType::INVALID_INPUT,
				                StringUtil::Format("role name \"%s\" is reserved", granted->name.GetIdentifierName()));
			}
			RequireAdminOf(caller, *granted, RoleChange::MEMBERS);
		} else {
			CheckMembershipAuthorization(context, caller, *granted, info.revoke);
		}
		info.grantor_ids.push_back(ResolveMembershipGrantor(binder, caller, *granted, info));
		for (auto &member : members) {
			if (!info.revoke && member.get().oid == DBConfig::GetConfig(context).database_owner_role) {
				throw NotImplementedException("role \"%s\" cannot be a member of any role",
				                              member.get().name.GetIdentifierName());
			}
		}
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
	if (rename) {
		RequireAdminOf(caller, role, RoleChange::RENAME);
		return;
	}
	if (caller.is_superuser) {
		return;
	}
	const bool admin = caller.Has(RoleOption::CreateRole) && caller.AdminOf(role.oid);
	if (role.IsSuperuser()) {
		throw PermissionException::WithDetail(
		    "Only roles with the SUPERUSER attribute may alter roles with the SUPERUSER attribute.",
		    "permission denied to alter role");
	}
	const auto changed = info.set_options | info.clear_options;
	if (HasOption(changed, RoleOption::Superuser)) {
		throw PermissionException::WithDetail(
		    "Only roles with the SUPERUSER attribute may change the SUPERUSER attribute.",
		    "permission denied to alter role");
	}
	const bool other = role.oid != caller.role;
	const auto not_admin = StringUtil::Format(
	    "Only roles with the CREATEROLE attribute and the ADMIN option on role \"%s\" may alter this role.", name);
	if (!admin && (changed != RoleOption::None || info.set_conn_limit || info.set_valid_until)) {
		throw PermissionException::WithDetail(not_admin, "permission denied to alter role");
	}
	if (!admin && other && (info.set_password || info.null_password)) {
		throw PermissionException::WithDetail("To change another role's password, the current user must have the "
		                                      "CREATEROLE attribute and the ADMIN option on the role.",
		                                      "permission denied to alter role");
	}
	if (!admin && other) {
		throw PermissionException::WithDetail(not_admin, "permission denied to alter role");
	}
	const auto attributes = changed & (RoleOption::CreateDb | RoleOption::Replication | RoleOption::BypassRls);
	for (auto &named : ROLE_OPTIONS) {
		if (HasOption(attributes, named.option) && !caller.Has(named.option)) {
			throw PermissionException::WithDetail(
			    StringUtil::Format("Only roles with the %s attribute may change the %s attribute.", named.keyword,
			                       named.keyword),
			    "permission denied to alter role");
		}
	}
}

void Binder::BindCreateRole(CreateRoleInfo &info) {
	auto &caller = CheckAs();
	const auto attributes =
	    info.options & (RoleOption::Superuser | RoleOption::CreateDb | RoleOption::Replication | RoleOption::BypassRls);
	for (auto &named : ROLE_OPTIONS) {
		if (!caller.is_superuser && HasOption(attributes, named.option) && !caller.Has(named.option)) {
			throw PermissionException::WithDetail(
			    StringUtil::Format("Only roles with the %s attribute may create roles with the %s attribute.",
			                       named.keyword, named.keyword),
			    "permission denied to create role");
		}
	}
	const auto resolve = [&](RoleSpec &spec) {
		auto role = BindRole(spec, OnEntryNotFound::RETURN_NULL);
		if (role && spec.type != RoleSpecType::NAME) {
			spec = RoleSpec(role->name);
		}
		return role;
	};
	for (auto &spec : info.in_roles) {
		if (auto granted = resolve(spec)) {
			CheckMembershipAuthorization(context, caller, *granted, false);
		}
	}
	const auto database_owner = DBConfig::GetConfig(context).database_owner_role;
	for (auto members : {&info.role_members, &info.admin_members}) {
		for (auto &spec : *members) {
			auto member = resolve(spec);
			if (member && member->oid == database_owner) {
				throw NotImplementedException("role \"%s\" cannot be a member of any role",
				                              member->name.GetIdentifierName());
			}
		}
	}
}

void Binder::BindAlterRole(AlterRoleInfo &info) {
	auto &caller = CheckAs();
	if (!info.granted_roles.empty()) {
		BindMemberships(*this, caller, info);
		return;
	}
	auto role = BindRole(info.Target(), OnEntryNotFound::RETURN_NULL);
	if (role) {
		info.SetQualifiedName(Identifier(), Identifier(), role->name);
		info.role_type = RoleSpecType::NAME;
	}
	if (role && !caller.IsSystem()) {
		CheckAlterRole(context, caller, *role, info);
	}
	const bool settings = !info.set_config.empty() || !info.reset_config.empty() || info.reset_all_config;
	if (settings && !caller.IsSystem()) {
		CheckConfig(context, info.set_config, info.reset_config);
	}
	if (settings && role && !caller.is_superuser) {
		CheckConfigChange(context, caller, info.set_config, info.reset_config, info.reset_all_config,
		                  role->GetInfo()->Cast<CreateRoleInfo>().config);
	}
	Settings::ResolveCurrentValues(context, info.set_config);
}

static void CheckDatabaseSettings(ClientContext &context, const RoleAccess &caller, optional_ptr<RoleEntry> role,
                                  DatabaseEntry &database, AlterDatabaseSettingsInfo &info) {
	if (role && role->IsPredefined()) {
		throw Exception({{"error_subtype", "RESERVED_NAME"}, {"detail", "Cannot alter reserved roles."}},
		                ExceptionType::INVALID_INPUT,
		                StringUtil::Format("role name \"%s\" is reserved", role->name.GetIdentifierName()));
	}
	if (role && role->oid != caller.role) {
		RequireAdminOf(caller, *role, RoleChange::ALTER);
	}
	CheckConfig(context, info.set_config, info.reset_config);
	if (caller.is_superuser) {
		return;
	}
	auto current = database.SettingsOf(info.role_id);
	CheckConfigChange(context, caller, info.set_config, info.reset_config, info.reset_all_config,
	                  current ? *current : vector<string>());
}

void Binder::BindAlterDatabaseSettings(AlterDatabaseSettingsInfo &info) {
	auto &caller = CheckAs();
	optional_ptr<RoleEntry> role;
	if (!info.role.empty()) {
		role = BindRole(info.role);
		info.role_id = role->oid;
	}
	auto database = entry_retriever.GetEntry(
	    EntryLookupInfo(CatalogType::DATABASE_ENTRY, info.GetQualifiedName(), role ? AclMode::NoRights : AclMode::Own));
	if (!caller.IsSystem()) {
		CheckDatabaseSettings(context, caller, role, database->Cast<DatabaseEntry>(), info);
	}
	Settings::ResolveCurrentValues(context, info.set_config);
}

void Binder::BindDropRole(const Identifier &name) {
	auto &caller = CheckAs();
	optional_ptr<RoleEntry> role;
	if (!caller.IsSystem()) {
		role = BindRole(RoleSpec(name), OnEntryNotFound::RETURN_NULL);
	}
	if (role) {
		RequireAdminOf(caller, *role, RoleChange::DROP);
	}
	if (role && role->IsPredefined()) {
		throw DependencyException("cannot drop role %s because it is required by the database system",
		                          role->name.GetIdentifierName());
	}
}

} // namespace duckdb
