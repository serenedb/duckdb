#include "duckdb/catalog/permissions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"

#include <algorithm>
#include <bit>

namespace duckdb {

CatalogType Permissions::AclClass(CatalogType entry_type) {
	switch (entry_type) {
	case CatalogType::VIEW_ENTRY:
		return CatalogType::TABLE_ENTRY;
	case CatalogType::TABLE_MACRO_ENTRY:
		return CatalogType::MACRO_ENTRY;
	default:
		return entry_type;
	}
}

AclMode Permissions::AllPrivileges(CatalogType acl_class) {
	switch (acl_class) {
	case CatalogType::TABLE_ENTRY:
		return AclMode::Select | AclMode::Insert | AclMode::Update | AclMode::Delete | AclMode::Truncate |
		       AclMode::References | AclMode::Trigger | AclMode::Maintain;
	case CatalogType::SEQUENCE_ENTRY:
		return AclMode::Select | AclMode::Update | AclMode::Usage;
	case CatalogType::DATABASE_ENTRY:
		return AclMode::Create | AclMode::CreateTemp | AclMode::Connect;
	case CatalogType::SCHEMA_ENTRY:
		return AclMode::Usage | AclMode::Create;
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY:
		return AclMode::Execute;
	case CatalogType::TYPE_ENTRY:
	case CatalogType::FOREIGN_SERVER_ENTRY:
	case CatalogType::FOREIGN_DATA_WRAPPER_ENTRY:
		return AclMode::Usage;
	case CatalogType::PARAMETER_ENTRY:
		return AclMode::Set | AclMode::AlterSystem;
	default:
		return AclMode::NoRights;
	}
}

AclMode Permissions::PublicPrivileges(CatalogType acl_class) {
	switch (acl_class) {
	case CatalogType::DATABASE_ENTRY:
		return AclMode::Connect | AclMode::CreateTemp;
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY:
		return AclMode::Execute;
	case CatalogType::TYPE_ENTRY:
		return AclMode::Usage;
	default:
		return AclMode::NoRights;
	}
}

bool Permissions::TryParsePrivilege(std::string_view keyword, CatalogType acl_class, AclMode &result) {
	static const case_insensitive_map_view_t<AclMode> names {
	    {"select", AclMode::Select},     {"insert", AclMode::Insert},
	    {"update", AclMode::Update},     {"delete", AclMode::Delete},
	    {"truncate", AclMode::Truncate}, {"references", AclMode::References},
	    {"trigger", AclMode::Trigger},   {"maintain", AclMode::Maintain},
	    {"execute", AclMode::Execute},   {"usage", AclMode::Usage},
	    {"create", AclMode::Create},     {"temporary", AclMode::CreateTemp},
	    {"temp", AclMode::CreateTemp},   {"connect", AclMode::Connect},
	    {"set", AclMode::Set},           {"alter system", AclMode::AlterSystem},
	};
	const auto allowed = AllPrivileges(acl_class);
	if (StringUtil::CIEquals(keyword, "ALL")) {
		result = allowed;
		return true;
	}
	auto entry = names.find(keyword);
	if (entry == names.end() || (allowed & entry->second) != entry->second) {
		return false;
	}
	result = entry->second;
	return true;
}

vector<AclItem> Permissions::AclDefault(CatalogType acl_class, idx_t owner) {
	vector<AclItem> acl;
	const auto owner_privs = AllPrivileges(acl_class);
	if (owner_privs == AclMode::NoRights) {
		return acl;
	}
	acl.push_back(AclItem {owner, owner, owner_privs, AclMode::NoRights});
	const auto public_privs = PublicPrivileges(acl_class);
	if (public_privs != AclMode::NoRights) {
		acl.push_back(AclItem {ACL_ID_PUBLIC, owner, public_privs, AclMode::NoRights});
	}
	return acl;
}

std::span<const AclItem> Permissions::ForeignDataWrapperAcl(idx_t wrapper) const {
	for (auto &row : foreign_data_wrappers) {
		if (row.oid == wrapper) {
			return row.acl;
		}
	}
	return {};
}

std::span<const AclItem> Permissions::ParameterAcl(std::string_view parameter) const {
	for (auto &row : parameters) {
		if (StringUtil::CIEquals(row.name, parameter)) {
			return row.acl;
		}
	}
	return {};
}

static AclMode HeldBy(const vector<AclItem> &acl, idx_t role, AclMode AclItem::*field) {
	AclMode held = AclMode::NoRights;
	for (auto &item : acl) {
		if (item.grantee == role) {
			held |= item.*field;
		}
	}
	return held;
}

static void Grant(vector<AclItem> &acl, idx_t grantee, idx_t grantor, AclMode privs, AclMode options) {
	for (auto &item : acl) {
		if (item.grantee == grantee && item.grantor == grantor) {
			item.privs |= privs;
			item.grant_option |= options;
			return;
		}
	}
	acl.push_back(AclItem {grantee, grantor, privs, options});
}

static void Revoke(vector<AclItem> &acl, idx_t grantee, idx_t grantor, AclMode privs, bool option_only, bool cascade) {
	AclMode lost = AclMode::NoRights;
	for (auto &item : acl) {
		if (item.grantee != grantee || item.grantor != grantor) {
			continue;
		}
		lost |= item.grant_option & privs;
		if (!option_only) {
			item.privs &= ~privs;
		}
		item.grant_option &= ~privs;
	}
	lost &= ~HeldBy(acl, grantee, &AclItem::grant_option);
	if (lost != AclMode::NoRights) {
		vector<idx_t> dependents;
		for (auto &item : acl) {
			if (item.grantor == grantee && item.grantee != grantee && (item.privs & lost) != AclMode::NoRights &&
			    std::find(dependents.begin(), dependents.end(), item.grantee) == dependents.end()) {
				dependents.push_back(item.grantee);
			}
		}
		if (!dependents.empty() && !cascade) {
			throw DependencyException("dependent privileges exist");
		}
		for (auto dependent : dependents) {
			Revoke(acl, dependent, grantee, lost, false, true);
		}
	}
	acl.erase(std::remove_if(acl.begin(), acl.end(),
	                         [](const AclItem &item) {
		                         return item.privs == AclMode::NoRights && item.grant_option == AclMode::NoRights;
	                         }),
	          acl.end());
}

static void Apply(vector<AclItem> &acl, idx_t grantor, AclMode grantable, AclMode privileges,
                  const AlterPermissionsInfo &info) {
	if (info.revoke) {
		Revoke(acl, info.grantee_id, grantor, privileges, info.option_only, info.cascade);
		return;
	}
	const auto granted = privileges & grantable;
	if (granted != AclMode::NoRights) {
		Grant(acl, info.grantee_id, grantor, granted, info.with_grant_option ? granted : AclMode::NoRights);
	}
}

static void ApplyGrant(const AlterPermissionsInfo &info, idx_t owner, vector<AclItem> &acl, CatalogType acl_class,
                       optional_ptr<ColumnList> columns) {
	auto stored = acl.empty() ? Permissions::AclDefault(acl_class, owner) : acl;
	idx_t grantor = owner;
	bool is_owner = true;
	AclMode grantable = Permissions::AllPrivileges(acl_class);
	if (!info.grantors.empty() && std::find(info.grantors.begin(), info.grantors.end(), owner) == info.grantors.end()) {
		grantor = info.grantors[0];
		is_owner = false;
		grantable = AclMode::NoRights;
		for (auto role : info.grantors) {
			auto held = HeldBy(stored, role, &AclItem::grant_option);
			if (columns) {
				for (auto &column : columns->Logical()) {
					held |= HeldBy(column.Acl(), role, &AclItem::grant_option);
				}
			}
			if (held != AclMode::NoRights) {
				grantor = role;
				grantable = HeldBy(stored, role, &AclItem::grant_option);
				break;
			}
		}
	}

	if (info.privileges != AclMode::NoRights) {
		Apply(stored, grantor, grantable, info.privileges, info);
		acl = std::move(stored);
		if (info.revoke && columns && (info.privileges & ACL_COLUMN_PRIVILEGES) != AclMode::NoRights) {
			for (auto &column : columns->Logical()) {
				if (column.Acl().empty()) {
					continue;
				}
				auto column_acl = column.Acl();
				Revoke(column_acl, info.grantee_id, grantor, info.privileges & ACL_COLUMN_PRIVILEGES, info.option_only,
				       info.cascade);
				columns->GetColumnMutable(column.Logical()).SetAcl(std::move(column_acl));
			}
		}
	}

	if (info.column_privileges.empty()) {
		return;
	}
	if (!columns) {
		throw InvalidInputException("column privileges are only valid for relations");
	}
	for (auto &column_privilege : info.column_privileges) {
		auto column_name = column_privilege.column;
		auto &column = columns->GetColumnMutable(columns->GetColumnIndex(column_name));
		auto column_acl = column.Acl();
		const auto column_grantable =
		    is_owner ? ACL_COLUMN_PRIVILEGES
		             : (grantable | HeldBy(column_acl, grantor, &AclItem::grant_option)) & ACL_COLUMN_PRIVILEGES;
		Apply(column_acl, grantor, column_grantable, column_privilege.privileges & ACL_COLUMN_PRIVILEGES, info);
		column.SetAcl(std::move(column_acl));
	}
}

void AlterPermissionsInfo::ApplyTo(Permissions &permissions, CatalogType entry_type,
                                   optional_ptr<ColumnList> columns) const {
	auto &info = *this;
	auto &owner = permissions.owner;
	auto &acl = permissions.acl;
	auto &defaults = permissions.defaults;
	if (info.drop_owned_role_id) {
		const auto role = info.drop_owned_role_id;
		for (auto &row : defaults) {
			Revoke(row.acl, role, row.role, ~AclMode::NoRights, false, true);
		}
		defaults.erase(std::remove_if(defaults.begin(), defaults.end(),
		                              [&](const DefaultAcl &row) {
			                              return row.role == role ||
			                                     row.acl == (row.scope == 0
			                                                     ? Permissions::AclDefault(row.objtype, row.role)
			                                                     : vector<AclItem> {});
		                              }),
		               defaults.end());
		return;
	}
	if (!info.new_owner.empty()) {
		const auto old_owner = owner;
		if (info.new_owner_id == old_owner) {
			return;
		}
		acl.erase(
		    std::remove_if(acl.begin(), acl.end(),
		                   [&](const AclItem &item) { return item.grantee == old_owner && item.grantor == old_owner; }),
		    acl.end());
		for (auto &item : acl) {
			if (item.grantor == old_owner) {
				item.grantor = info.new_owner_id;
			}
		}
		owner = info.new_owner_id;
		return;
	}

	if (info.default_objtype != CatalogType::INVALID) {
		auto row = std::find_if(defaults.begin(), defaults.end(), [&](const DefaultAcl &entry) {
			return entry.role == info.target_role && entry.scope == info.default_scope &&
			       entry.objtype == info.default_objtype;
		});
		const auto baseline = info.default_scope == 0 ? Permissions::AclDefault(info.default_objtype, info.target_role)
		                                              : vector<AclItem> {};
		auto row_acl = row == defaults.end() ? baseline : row->acl;
		Apply(row_acl, info.target_role, Permissions::AllPrivileges(info.default_objtype), info.privileges, info);
		if (row_acl == baseline) {
			if (row != defaults.end()) {
				defaults.erase(row);
			}
		} else if (row == defaults.end()) {
			defaults.push_back(
			    DefaultAcl {info.target_role, info.default_scope, info.default_objtype, std::move(row_acl)});
		} else {
			row->acl = std::move(row_acl);
		}
		return;
	}

	if (info.foreign_data_wrapper_id) {
		auto &wrappers = permissions.foreign_data_wrappers;
		auto row = std::find_if(wrappers.begin(), wrappers.end(),
		                        [&](const ObjectAcl &entry) { return entry.oid == info.foreign_data_wrapper_id; });
		auto row_acl = row == wrappers.end() ? vector<AclItem> {} : row->acl;
		ApplyGrant(info, info.object_owner_id, row_acl, CatalogType::FOREIGN_DATA_WRAPPER_ENTRY, nullptr);
		if (row_acl.empty()) {
			if (row != wrappers.end()) {
				wrappers.erase(row);
			}
		} else if (row == wrappers.end()) {
			wrappers.push_back(ObjectAcl {info.foreign_data_wrapper_id, std::move(row_acl)});
		} else {
			row->acl = std::move(row_acl);
		}
		return;
	}

	if (!info.parameter.empty()) {
		auto &rows = permissions.parameters;
		auto row = std::find_if(rows.begin(), rows.end(), [&](const NamedAcl &entry) {
			return StringUtil::CIEquals(entry.name, info.parameter);
		});
		auto row_acl = row == rows.end() ? vector<AclItem> {} : row->acl;
		ApplyGrant(info, info.object_owner_id, row_acl, CatalogType::PARAMETER_ENTRY, nullptr);
		if (row_acl.empty() || row_acl == Permissions::AclDefault(CatalogType::PARAMETER_ENTRY, info.object_owner_id)) {
			if (row != rows.end()) {
				rows.erase(row);
			}
		} else if (row == rows.end()) {
			rows.push_back(NamedAcl {info.parameter, std::move(row_acl)});
		} else {
			row->acl = std::move(row_acl);
		}
		return;
	}

	ApplyGrant(info, owner, acl, Permissions::AclClass(entry_type), columns);
}

void Permissions::AssignOwner(idx_t new_owner, CatalogType objtype, optional_ptr<const Permissions> database,
                              optional_idx schema) {
	owner = new_owner;
	if (!database) {
		return;
	}
	auto row = [&](idx_t scope) -> optional_ptr<const DefaultAcl> {
		for (auto &defaults_entry : database->defaults) {
			if (defaults_entry.role == new_owner && defaults_entry.objtype == objtype &&
			    defaults_entry.scope == scope) {
				return &defaults_entry;
			}
		}
		return nullptr;
	};
	auto global = row(0);
	optional_ptr<const DefaultAcl> in_schema;
	if (schema.IsValid()) {
		in_schema = row(schema.GetIndex());
	}
	if (!global && !in_schema) {
		return;
	}
	auto granted = global ? global->acl : AclDefault(objtype, new_owner);
	if (in_schema) {
		for (auto &item : in_schema->acl) {
			Grant(granted, item.grantee, item.grantor, item.privs, item.grant_option);
		}
	}
	acl = std::move(granted);
}

static bool FilterMayContain(std::span<const uint64_t> filter, idx_t member) {
	const auto hash = Hash(uint64_t(member));
	const auto mask = filter.size() * 64 - 1;
	const auto first = hash & mask;
	const auto second = (hash >> 32) & mask;
	return ((filter[first >> 6] >> (first & 63)) & (filter[second >> 6] >> (second & 63)) & 1) != 0;
}

static idx_t FilterWords(idx_t members) {
	return std::bit_ceil(std::max<idx_t>(1, (members + 3) / 4));
}

const RoleAccess &RoleAccess::System() {
	static const RoleAccess system = [] {
		RoleAccess access;
		access.options = ~RoleOption::None;
		access.is_superuser = true;
		return access;
	}();
	return system;
}

bool RoleAccess::Uses(idx_t other) const {
	return other == role || (!closure_filter.empty() && FilterMayContain(closure_filter, other) &&
	                         std::binary_search(closure.begin(), closure.end(), other));
}

bool RoleAccess::CanSet(idx_t other) const {
	return is_superuser || other == role || std::binary_search(settable.begin(), settable.end(), other);
}

static bool Reaches(const RoleAccess &access, const AclItem &item) {
	return item.grantee == ACL_ID_PUBLIC || access.Uses(item.grantee);
}

static AclMode Held(const RoleAccess &access, std::span<const AclItem> acl, AclMode AclItem::*field) {
	AclMode held = AclMode::NoRights;
	for (auto &item : acl) {
		if (Reaches(access, item)) {
			held |= item.*field;
		}
	}
	return held;
}

static AclMode CapabilityModes(const RoleAccess &access, CatalogType type) {
	AclMode modes = AclMode::NoRights;
	if (access.capabilities == RoleCapability::None) {
		return modes;
	}
	const auto has = [&](RoleCapability capability) {
		return (access.capabilities & capability) == capability;
	};
	switch (type) {
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
	case CatalogType::SEQUENCE_ENTRY:
		if (has(RoleCapability::ReadAllData)) {
			modes |= AclMode::Select;
		}
		if (has(RoleCapability::WriteAllData)) {
			modes |= AclMode::Insert | AclMode::Update | AclMode::Delete;
		}
		if (has(RoleCapability::Maintain) && type != CatalogType::SEQUENCE_ENTRY) {
			modes |= AclMode::Maintain;
		}
		break;
	case CatalogType::SCHEMA_ENTRY:
		if (has(RoleCapability::ReadAllData) || has(RoleCapability::WriteAllData)) {
			modes |= AclMode::Usage;
		}
		break;
	default:
		break;
	}
	return modes;
}

static bool Allows(const RoleAccess &access, std::span<const AclItem> stored, CatalogType type, AclMode need,
                   bool any) {
	if (need == AclMode::NoRights) {
		return false;
	}
	const auto done = [&](AclMode have) {
		return any ? (have & need) != AclMode::NoRights : (have & need) == need;
	};
	AclMode have = CapabilityModes(access, type);
	if (stored.empty()) {
		return done(have | Permissions::PublicPrivileges(type));
	}
	if (done(have)) {
		return true;
	}
	for (auto &item : stored) {
		if (Reaches(access, item)) {
			have |= item.privs;
			if (done(have)) {
				return true;
			}
		}
	}
	return false;
}

bool RoleAccess::Can(CatalogType type, const Permissions &permissions, AclMode need) const {
	return Owns(permissions.owner) || Allows(*this, permissions.acl, type, need, false);
}

bool RoleAccess::CanAny(CatalogType type, const Permissions &permissions, AclMode need) const {
	return Owns(permissions.owner) || Allows(*this, permissions.acl, type, need, true);
}

bool RoleAccess::CanColumns(const Permissions &permissions, AclMode need,
                            std::span<const std::span<const AclItem>> acls) const {
	if (Can(CatalogType::TABLE_ENTRY, permissions, need)) {
		return true;
	}
	return !acls.empty() && std::all_of(acls.begin(), acls.end(), [&](std::span<const AclItem> acl) {
		return Allows(*this, acl, CatalogType::TABLE_ENTRY, need, false);
	});
}

bool RoleAccess::CanAnyColumn(const Permissions &permissions, AclMode need, const ColumnList &columns) const {
	if (Can(CatalogType::TABLE_ENTRY, permissions, need)) {
		return true;
	}
	for (auto &column : columns.Logical()) {
		if (Allows(*this, column.Acl(), CatalogType::TABLE_ENTRY, need, false)) {
			return true;
		}
	}
	return false;
}

AclMode RoleAccess::HeldModes(std::span<const AclItem> acl) const {
	return Held(*this, acl, &AclItem::privs);
}

AclMode RoleAccess::GrantableModes(std::span<const AclItem> acl) const {
	return Held(*this, acl, &AclItem::grant_option);
}

SecuritySnapshot::SecuritySnapshot(vector<Role> roles_p) {
	std::sort(roles_p.begin(), roles_p.end(), [](const Role &lhs, const Role &rhs) { return lhs.role < rhs.role; });
	idx_t total = 0;
	for (auto &role : roles_p) {
		role.closure.erase(std::remove(role.closure.begin(), role.closure.end(), role.role), role.closure.end());
		for (auto list : {&role.closure, &role.settable}) {
			std::sort(list->begin(), list->end());
			list->erase(std::unique(list->begin(), list->end()), list->end());
			total += list->size();
		}
		if (!role.closure.empty()) {
			total += FilterWords(role.closure.size());
		}
	}
	arena.reserve(total);
	roles.reserve(roles_p.size());
	for (auto &role : roles_p) {
		RoleAccess access;
		access.role = role.role;
		access.options = role.options;
		access.capabilities = role.capabilities;
		access.is_superuser = HasOption(role.options, RoleOption::Superuser);
		if (!role.closure.empty()) {
			const auto words = FilterWords(role.closure.size());
			const auto filter_begin = arena.size();
			arena.resize(filter_begin + words, 0);
			const auto mask = words * 64 - 1;
			for (auto member : role.closure) {
				const auto hash = Hash(uint64_t(member));
				const auto first = hash & mask;
				const auto second = (hash >> 32) & mask;
				arena[filter_begin + (first >> 6)] |= uint64_t(1) << (first & 63);
				arena[filter_begin + (second >> 6)] |= uint64_t(1) << (second & 63);
			}
			access.closure_filter = std::span<const uint64_t>(arena.data() + filter_begin, words);
		}
		const auto begin = arena.size();
		arena.insert(arena.end(), role.closure.begin(), role.closure.end());
		access.closure = std::span<const idx_t>(arena.data() + begin, role.closure.size());
		const auto settable_begin = arena.size();
		arena.insert(arena.end(), role.settable.begin(), role.settable.end());
		access.settable = std::span<const idx_t>(arena.data() + settable_begin, role.settable.size());
		roles.push_back(access);
	}
}

SecuritySnapshot::~SecuritySnapshot() {
}

optional_ptr<const RoleAccess> SecuritySnapshot::Access(idx_t role) const {
	auto it = std::lower_bound(roles.begin(), roles.end(), role,
	                           [](const RoleAccess &access, idx_t value) { return access.role < value; });
	if (it == roles.end() || it->role != role) {
		return nullptr;
	}
	return &*it;
}

bool IsUnowned(const CatalogEntry &entry) {
	auto &catalog = entry.ParentCatalog();
	if (!entry.internal && catalog.GetAttached().IsReadOnly()) {
		return true;
	}
	const bool stores_owners = catalog.Compatibility() == SqlCompatibility::POSTGRES;
	return entry.permissions.owner == 0 && (entry.internal || !stores_owners);
}

bool CanUseSchema(const RoleAccess &role, const CatalogEntry &schema) {
	return role.is_superuser || schema.internal || IsUnowned(schema) ||
	       schema.ParentCatalog().Compatibility() != SqlCompatibility::POSTGRES ||
	       role.Can(CatalogType::SCHEMA_ENTRY, schema.permissions, AclMode::Usage);
}

template <class F>
static bool AnyRoleReference(const CatalogEntry &entry, F &&named) {
	auto &permissions = entry.permissions;
	const auto any_acl = [&](const vector<AclItem> &acl) {
		return std::any_of(acl.begin(), acl.end(),
		                   [&](const AclItem &item) { return named(item.grantee) || named(item.grantor); });
	};
	if (named(permissions.owner) || any_acl(permissions.acl)) {
		return true;
	}
	for (auto &row : permissions.defaults) {
		if (named(row.role) || any_acl(row.acl)) {
			return true;
		}
	}
	for (auto &row : permissions.foreign_data_wrappers) {
		if (any_acl(row.acl)) {
			return true;
		}
	}
	for (auto &row : permissions.parameters) {
		if (any_acl(row.acl)) {
			return true;
		}
	}
	if (entry.type == CatalogType::VIEW_ENTRY) {
		auto &column_acls = entry.Cast<ViewCatalogEntry>().column_acls;
		return std::any_of(column_acls.begin(), column_acls.end(),
		                   [&](const vector<AclItem> &acl) { return any_acl(acl); });
	}
	if (entry.type != CatalogType::TABLE_ENTRY) {
		return false;
	}
	for (auto &column : entry.Cast<TableCatalogEntry>().GetColumns().Logical()) {
		if (any_acl(column.Acl())) {
			return true;
		}
	}
	return false;
}

bool ReferencesRole(const CatalogEntry &entry, idx_t role) {
	return AnyRoleReference(entry, [&](idx_t oid) { return oid == role; });
}

void ForEachRoleReference(const CatalogEntry &entry, const std::function<void(idx_t role)> &callback) {
	AnyRoleReference(entry, [&](idx_t oid) {
		if (oid != ACL_ID_PUBLIC) {
			callback(oid);
		}
		return false;
	});
}

vector<idx_t> RoleReferences(const CatalogEntry &entry) {
	vector<idx_t> roles;
	ForEachRoleReference(entry, [&](idx_t role) { roles.push_back(role); });
	std::sort(roles.begin(), roles.end());
	roles.erase(std::unique(roles.begin(), roles.end()), roles.end());
	return roles;
}

string PermissionKindName(CatalogType type) {
	switch (type) {
	case CatalogType::TABLE_ENTRY:
		return "table";
	case CatalogType::VIEW_ENTRY:
		return "view";
	case CatalogType::SEQUENCE_ENTRY:
		return "sequence";
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY:
	case CatalogType::TABLE_FUNCTION_ENTRY:
	case CatalogType::SCALAR_FUNCTION_ENTRY:
		return "function";
	case CatalogType::TYPE_ENTRY:
		return "type";
	case CatalogType::SCHEMA_ENTRY:
		return "schema";
	case CatalogType::DATABASE_ENTRY:
		return "database";
	case CatalogType::INDEX_ENTRY:
		return "index";
	case CatalogType::TOKENIZER_ENTRY:
		return "text search dictionary";
	case CatalogType::FOREIGN_SERVER_ENTRY:
		return "foreign server";
	case CatalogType::FOREIGN_DATA_WRAPPER_ENTRY:
		return "foreign-data wrapper";
	case CatalogType::PARAMETER_ENTRY:
		return "parameter";
	case CatalogType::SUBSCRIPTION_ENTRY:
		return "subscription";
	case CatalogType::JOB_ENTRY:
		return "job";
	default:
		return "object";
	}
}

string PermissionKindName(const CatalogEntry &entry) {
	if ((entry.type == CatalogType::MACRO_ENTRY || entry.type == CatalogType::TABLE_MACRO_ENTRY) &&
	    entry.Cast<MacroCatalogEntry>().is_procedure) {
		return "procedure";
	}
	return PermissionKindName(entry.type);
}

void ThrowPermissionDenied(const CatalogEntry &entry) {
	throw PermissionException("permission denied for %s %s", PermissionKindName(entry), entry.name.GetIdentifierName());
}

} // namespace duckdb
