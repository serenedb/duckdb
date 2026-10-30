#include "duckdb/catalog/permissions.hpp"

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"

#include <algorithm>

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
		return AclMode::Usage;
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
	    {"select", AclMode::Select},   {"insert", AclMode::Insert},     {"update", AclMode::Update},
	    {"delete", AclMode::Delete},   {"truncate", AclMode::Truncate}, {"references", AclMode::References},
	    {"trigger", AclMode::Trigger}, {"maintain", AclMode::Maintain}, {"execute", AclMode::Execute},
	    {"usage", AclMode::Usage},     {"create", AclMode::Create},     {"temporary", AclMode::CreateTemp},
	    {"temp", AclMode::CreateTemp}, {"connect", AclMode::Connect},
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

void AlterPermissionsInfo::ApplyTo(Permissions &permissions, CatalogType entry_type,
                                   optional_ptr<ColumnList> columns) const {
	auto &info = *this;
	auto &owner = permissions.owner;
	auto &acl = permissions.acl;
	auto &defaults = permissions.defaults;
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

	const auto acl_class = Permissions::AclClass(entry_type);
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

bool RoleAccess::Uses(idx_t other) const {
	return other == role ||
	       (((closure_mask >> (other & 63)) & 1) != 0 && std::binary_search(closure.begin(), closure.end(), other));
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

static bool Allows(const RoleAccess &access, std::span<const AclItem> stored, CatalogType type, idx_t owner,
                   AclMode need, bool any) {
	if (need == AclMode::NoRights) {
		return false;
	}
	const auto done = [&](AclMode have) {
		return any ? (have & need) != AclMode::NoRights : (have & need) == need;
	};
	AclMode have = AclMode::NoRights;
	if (access.Uses(owner)) {
		have |= Permissions::AllPrivileges(type);
		if (done(have)) {
			return true;
		}
	}
	if (stored.empty()) {
		return done(have | Permissions::PublicPrivileges(type));
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
	return Owns(permissions.owner) || Allows(*this, permissions.acl, type, permissions.owner, need, false);
}

bool RoleAccess::CanAny(CatalogType type, const Permissions &permissions, AclMode need) const {
	return Owns(permissions.owner) || Allows(*this, permissions.acl, type, permissions.owner, need, true);
}

bool RoleAccess::CanColumns(const Permissions &permissions, AclMode need,
                            std::span<const std::span<const AclItem>> acls) const {
	if (Can(CatalogType::TABLE_ENTRY, permissions, need)) {
		return true;
	}
	return !acls.empty() && std::all_of(acls.begin(), acls.end(), [&](std::span<const AclItem> acl) {
		return Allows(*this, acl, CatalogType::TABLE_ENTRY, permissions.owner, need, false);
	});
}

bool RoleAccess::CanAnyColumn(const Permissions &permissions, AclMode need,
                              std::span<const std::span<const AclItem>> acls) const {
	if (Can(CatalogType::TABLE_ENTRY, permissions, need)) {
		return true;
	}
	return std::any_of(acls.begin(), acls.end(), [&](std::span<const AclItem> acl) {
		return Allows(*this, acl, CatalogType::TABLE_ENTRY, permissions.owner, need, false);
	});
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
		std::sort(role.closure.begin(), role.closure.end());
		role.closure.erase(std::unique(role.closure.begin(), role.closure.end()), role.closure.end());
		total += role.closure.size();
	}
	arena.reserve(total);
	roles.reserve(roles_p.size());
	for (auto &role : roles_p) {
		RoleAccess access;
		access.role = role.role;
		access.options = role.options;
		access.is_superuser = HasOption(role.options, RoleOption::Superuser);
		const auto begin = arena.size();
		for (auto member : role.closure) {
			arena.push_back(member);
			access.closure_mask |= uint64_t(1) << (member & 63);
		}
		access.closure = std::span<const idx_t>(arena.data() + begin, role.closure.size());
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

} // namespace duckdb
