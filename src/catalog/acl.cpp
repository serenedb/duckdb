#include "duckdb/catalog/acl.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"

#include <absl/algorithm/container.h>
#include <bit>

namespace duckdb {

AclMode FindPrivilege(std::string_view keyword) {
	if (StringUtil::CIEquals(keyword, "TEMP")) {
		return AclMode::CreateTemp;
	}
	auto privilege = absl::c_find_if(ACL_PRIVILEGES, [&](const AclPrivilege &candidate) {
		return StringUtil::CIEquals(keyword, candidate.keyword);
	});
	return privilege == std::end(ACL_PRIVILEGES) ? AclMode::NoRights : privilege->mode;
}

bool Permissions::TryParsePrivilege(std::string_view keyword, CatalogType acl_class, AclMode &result) {
	const auto allowed = ObjectKindOf(acl_class).privileges;
	if (StringUtil::CIEquals(keyword, "ALL")) {
		result = allowed;
		return true;
	}
	result = FindPrivilege(keyword);
	return result != AclMode::NoRights && (allowed & result) == result;
}

vector<AclItem> Permissions::AclDefault(CatalogType acl_class, idx_t owner) {
	vector<AclItem> acl;
	const auto kind = ObjectKindOf(acl_class);
	if (kind.privileges == AclMode::NoRights) {
		return acl;
	}
	if (kind.public_privileges != AclMode::NoRights) {
		acl.push_back(AclItem {ACL_ID_PUBLIC, owner, kind.public_privileges, AclMode::NoRights});
	}
	acl.push_back(AclItem {owner, owner, kind.privileges, AclMode::NoRights});
	return acl;
}

static AclMode HeldBy(std::span<const AclItem> acl, idx_t role, AclMode AclItem::*field) {
	AclMode held = AclMode::NoRights;
	for (auto &item : acl) {
		if (item.grantee == role) {
			held |= item.*field;
		}
	}
	return held;
}

Grantor BestGrantor(std::span<const idx_t> roles, idx_t owner, AclMode needed, std::span<const AclItem> acl,
                    std::span<const AclItem> column_acl) {
	if (roles.empty() || roles.front() == owner) {
		return {owner, needed};
	}
	Grantor best {roles.front(), AclMode::NoRights};
	int best_count = 0;
	for (auto role : roles) {
		const auto options =
		    role == owner
		        ? needed
		        : (HeldBy(acl, role, &AclItem::grant_option) | HeldBy(column_acl, role, &AclItem::grant_option)) &
		              needed;
		if (options == needed) {
			return {role, options};
		}
		const auto count = std::popcount(static_cast<uint64_t>(options));
		if (count > best_count) {
			best = {role, options};
			best_count = count;
		}
	}
	return best;
}

static void Grant(vector<AclItem> &acl, idx_t grantee, idx_t grantor, AclMode privs, AclMode options) {
	auto item = absl::c_find_if(
	    acl, [&](const AclItem &entry) { return entry.grantee == grantee && entry.grantor == grantor; });
	if (item == acl.end()) {
		acl.push_back(AclItem {grantee, grantor, privs, options});
		return;
	}
	item->privs |= privs;
	item->grant_option |= options;
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
			    !absl::c_linear_search(dependents, item.grantee)) {
				dependents.push_back(item.grantee);
			}
		}
		if (!dependents.empty() && !cascade) {
			throw Exception(unordered_map<string, string> {{"hint", "Use CASCADE to revoke them too."}},
			                ExceptionType::DEPENDENCY, "dependent privileges exist");
		}
		for (auto dependent : dependents) {
			Revoke(acl, dependent, grantee, lost, false, true);
		}
	}
	std::erase_if(acl, [](const AclItem &item) { return item.GrantsNothing(); });
}

static void ReplaceOwner(vector<AclItem> &acl, idx_t old_owner, idx_t new_owner) {
	vector<AclItem> replaced;
	for (auto item : acl) {
		if (item.grantee == old_owner) {
			item.grantee = new_owner;
		}
		if (item.grantor == old_owner) {
			item.grantor = new_owner;
		}
		Grant(replaced, item.grantee, item.grantor, item.privs, item.grant_option);
	}
	acl = std::move(replaced);
}

static void Apply(vector<AclItem> &acl, idx_t grantor, AclMode grantable, AclMode privileges,
                  const AlterPermissionsInfo &info) {
	if (info.revoke) {
		Revoke(acl, info.grantee_id, grantor, privileges, info.option_only, info.cascade);
		return;
	}
	if (info.with_grant_option && info.grantee_id == ACL_ID_PUBLIC) {
		throw Exception({{"error_subtype", "INVALID_GRANT_OPERATION"}}, ExceptionType::INVALID_INPUT,
		                "grant options can only be granted to roles");
	}
	const auto granted = privileges & grantable;
	if (granted != AclMode::NoRights) {
		Grant(acl, info.grantee_id, grantor, granted, info.with_grant_option ? granted : AclMode::NoRights);
	}
}

static vector<AclItem> &ColumnRow(vector<ColumnAcl> &columns, const Identifier &column) {
	auto row = absl::c_find_if(columns, [&](const ColumnAcl &entry) { return entry.column == column; });
	if (row != columns.end()) {
		return row->acl;
	}
	columns.push_back(ColumnAcl {column, {}});
	return columns.back().acl;
}

static void ApplyGrant(const AlterPermissionsInfo &info, Permissions &permissions, CatalogType acl_class,
                       optional_ptr<vector<ColumnAcl>> columns) {
	const auto owner = permissions.owner;
	auto stored = permissions.UsesDefaultAcl() ? Permissions::AclDefault(acl_class, owner) : permissions.acl;
	idx_t grantor = owner;
	if (!info.best_grantor && !info.grantors.empty() && !absl::c_linear_search(info.grantors, owner)) {
		const auto holder = absl::c_find_if(info.grantors, [&](idx_t role) {
			auto held = HeldBy(stored, role, &AclItem::grant_option);
			if (columns) {
				for (auto &column : *columns) {
					held |= HeldBy(column.acl, role, &AclItem::grant_option);
				}
			}
			return held != AclMode::NoRights;
		});
		grantor = holder == info.grantors.end() ? info.grantors[0] : *holder;
	}
	const auto choose = [&](AclMode needed, std::span<const AclItem> column_acl) {
		return BestGrantor(info.best_grantor ? std::span<const idx_t>(info.grantors)
		                                     : std::span<const idx_t>(&grantor, 1),
		                   owner, needed, stored, column_acl);
	};

	if (!info.column_privileges.empty() && !columns) {
		throw InvalidInputException("column privileges are only valid for relations");
	}
	if (columns) {
		for (auto &column_privilege : info.column_privileges) {
			ColumnRow(*columns, column_privilege.column);
		}
		const auto implied = info.revoke ? info.privileges & ACL_COLUMN_PRIVILEGES : AclMode::NoRights;
		for (auto &column : *columns) {
			auto privileges = implied;
			for (auto &column_privilege : info.column_privileges) {
				if (column_privilege.column == column.column) {
					privileges |= column_privilege.privileges & ACL_COLUMN_PRIVILEGES;
				}
			}
			if (privileges != AclMode::NoRights) {
				const auto chosen = choose(privileges, column.acl);
				Apply(column.acl, chosen.role, chosen.options, privileges, info);
			}
		}
		std::erase_if(*columns, [](const ColumnAcl &column) { return column.acl.empty(); });
	}

	if (info.privileges != AclMode::NoRights) {
		const auto chosen = choose(info.privileges, {});
		Apply(stored, chosen.role, chosen.options, info.privileges, info);
		permissions.acl = std::move(stored);
		permissions.acl_emptied = permissions.acl.empty();
	}
}

void AlterPermissionsInfo::ApplyTo(Permissions &permissions, CatalogType entry_type,
                                   optional_ptr<vector<ColumnAcl>> columns) const {
	auto &info = *this;
	D_ASSERT(!info.drop_owned_role_id && info.default_objtype == CatalogType::INVALID);
	auto &owner = permissions.owner;
	auto &acl = permissions.acl;
	if (!info.new_owner.empty()) {
		if (info.new_owner_id == owner) {
			return;
		}
		ReplaceOwner(acl, owner, info.new_owner_id);
		if (columns) {
			for (auto &column : *columns) {
				ReplaceOwner(column.acl, owner, info.new_owner_id);
			}
		}
		owner = info.new_owner_id;
		return;
	}
	ApplyGrant(info, permissions, ObjectKindOf(entry_type).acl_class, columns);
}

void AlterPermissionsInfo::ApplyToDatabase(CreatePermissions &permissions) const {
	auto &info = *this;
	auto &defaults = permissions.defaults;
	if (info.drop_owned_role_id) {
		const auto role = info.drop_owned_role_id;
		for (auto &row : defaults) {
			Revoke(row.acl, role, row.role, ~AclMode::NoRights, false, true);
		}
		std::erase_if(defaults, [&](const DefaultAcl &row) {
			const auto baseline = row.scope == 0 ? Permissions::AclDefault(row.objtype, row.role) : vector<AclItem> {};
			return row.role == role || absl::c_is_permutation(row.acl, baseline);
		});
		return;
	}
	if (info.default_objtype == CatalogType::INVALID) {
		ApplyTo(permissions, CatalogType::DATABASE_ENTRY, nullptr);
		return;
	}
	auto row = absl::c_find_if(defaults, [&](const DefaultAcl &entry) {
		return entry.role == info.target_role && entry.scope == info.default_scope &&
		       entry.objtype == info.default_objtype;
	});
	const auto baseline =
	    info.default_scope == 0 ? Permissions::AclDefault(info.default_objtype, info.target_role) : vector<AclItem> {};
	auto row_acl = row == defaults.end() ? baseline : row->acl;
	Apply(row_acl, info.target_role, ObjectKindOf(info.default_objtype).privileges, info.privileges, info);
	if (absl::c_is_permutation(row_acl, baseline)) {
		if (row != defaults.end()) {
			defaults.erase(row);
		}
	} else if (row == defaults.end()) {
		defaults.push_back(DefaultAcl {info.target_role, info.default_scope, info.default_objtype, std::move(row_acl),
		                               info.default_acl_oid});
	} else {
		row->acl = std::move(row_acl);
	}
}

void AlterPermissionsInfo::ApplyTo(Permissions &permissions, CatalogType entry_type, ColumnList &columns) const {
	vector<ColumnAcl> column_acls;
	for (auto &column : columns.Logical()) {
		if (!column.Acl().empty()) {
			column_acls.push_back(ColumnAcl {column.Name(), column.Acl()});
		}
	}
	ApplyTo(permissions, entry_type, &column_acls);
	for (auto &column : columns.Logical()) {
		const auto acl = FindColumnAcl(column_acls, column.Name());
		columns.GetColumnMutable(column.Logical()).SetAcl(vector<AclItem>(acl.begin(), acl.end()));
	}
}

void Permissions::AssignOwner(idx_t new_owner, CatalogType objtype, std::span<const DefaultAcl> defaults,
                              optional_idx schema) {
	owner = new_owner;
	auto row = [&](idx_t scope) -> optional_ptr<const DefaultAcl> {
		auto found = absl::c_find_if(defaults, [&](const DefaultAcl &entry) {
			return entry.role == new_owner && entry.objtype == objtype && entry.scope == scope;
		});
		return found == defaults.end() ? nullptr : &*found;
	};
	auto global = row(0);
	optional_ptr<const DefaultAcl> in_schema;
	if (schema.IsValid()) {
		in_schema = row(schema.GetIndex());
	}
	if (!global && !in_schema) {
		return;
	}
	const auto builtin = AclDefault(objtype, new_owner);
	auto granted = global ? global->acl : builtin;
	if (in_schema) {
		for (auto &item : in_schema->acl) {
			Grant(granted, item.grantee, item.grantor, item.privs, item.grant_option);
		}
	}
	if (absl::c_is_permutation(granted, builtin)) {
		granted.clear();
	}
	acl = std::move(granted);
}

std::span<const AclItem> FindColumnAcl(const vector<ColumnAcl> &columns, const Identifier &column) {
	auto row = absl::c_find_if(columns, [&](const ColumnAcl &entry) { return entry.column == column; });
	return row == columns.end() ? std::span<const AclItem> {} : std::span<const AclItem>(row->acl);
}

} // namespace duckdb
