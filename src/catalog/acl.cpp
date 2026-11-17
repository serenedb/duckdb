#include "duckdb/catalog/acl.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"

#include <algorithm>

namespace duckdb {

AclMode FindPrivilege(std::string_view keyword) {
	if (StringUtil::CIEquals(keyword, "TEMP")) {
		return AclMode::CreateTemp;
	}
	for (auto &privilege : ACL_PRIVILEGES) {
		if (StringUtil::CIEquals(keyword, privilege.keyword)) {
			return privilege.mode;
		}
	}
	return AclMode::NoRights;
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

bool Permissions::operator==(const Permissions &rhs) const {
	return owner == rhs.owner && acl == rhs.acl && defaults == rhs.defaults && Rows() == rhs.Rows();
}

const GrantRows &Permissions::Rows() const {
	static const GrantRows empty;
	return rows ? *rows : empty;
}

GrantRows &Permissions::MutableRows() {
	if (!rows) {
		rows = make_shared_ptr<GrantRows>();
	} else if (rows.use_count() > 1) {
		rows = make_shared_ptr<GrantRows>(*rows);
	}
	return *rows;
}

bool GrantRow::Is(CatalogType kind_p, idx_t oid_p, std::string_view name_p) const {
	if (kind != kind_p) {
		return false;
	}
	return kind == CatalogType::PARAMETER_ENTRY ? StringUtil::CIEquals(name, name_p) : oid == oid_p;
}

template <class ROWS>
static auto KindRange(ROWS &rows, CatalogType kind) {
	auto lower = std::lower_bound(rows.begin(), rows.end(), kind,
	                              [](const GrantRow &row, CatalogType value) { return row.kind < value; });
	auto upper = std::upper_bound(lower, rows.end(), kind,
	                              [](CatalogType value, const GrantRow &row) { return value < row.kind; });
	return std::make_pair(lower, upper);
}

std::span<const GrantRow> GrantRows::OfKind(CatalogType kind) const {
	auto range = KindRange(rows, kind);
	return std::span<const GrantRow>(range.first, range.second);
}

optional_ptr<const GrantRow> Permissions::FindRow(CatalogType kind, idx_t oid, std::string_view name) const {
	for (auto &row : Rows().OfKind(kind)) {
		if (row.Is(kind, oid, name)) {
			return row;
		}
	}
	return nullptr;
}

std::span<const AclItem> Permissions::RowAcl(CatalogType kind, idx_t oid, std::string_view name) const {
	auto row = FindRow(kind, oid, name);
	return row ? std::span<const AclItem>(row->acl) : std::span<const AclItem>();
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
	for (auto &row : columns) {
		if (row.column == column) {
			return row.acl;
		}
	}
	columns.push_back(ColumnAcl {column, {}});
	return columns.back().acl;
}

static void ApplyGrant(const AlterPermissionsInfo &info, idx_t owner, vector<AclItem> &acl, CatalogType acl_class,
                       optional_ptr<vector<ColumnAcl>> columns) {
	auto stored = acl.empty() ? Permissions::AclDefault(acl_class, owner) : acl;
	idx_t grantor = owner;
	bool is_owner = true;
	AclMode grantable = ObjectKindOf(acl_class).privileges;
	if (!info.grantors.empty() && std::find(info.grantors.begin(), info.grantors.end(), owner) == info.grantors.end()) {
		grantor = info.grantors[0];
		is_owner = false;
		grantable = AclMode::NoRights;
		for (auto role : info.grantors) {
			auto held = HeldBy(stored, role, &AclItem::grant_option);
			if (columns) {
				for (auto &column : *columns) {
					held |= HeldBy(column.acl, role, &AclItem::grant_option);
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
		if (acl.empty() && !Permissions::AclDefault(acl_class, owner).empty()) {
			acl.push_back(AclItem {owner, owner, AclMode::NoRights, AclMode::NoRights});
		}
		if (info.revoke && columns && (info.privileges & ACL_COLUMN_PRIVILEGES) != AclMode::NoRights) {
			for (auto &column : *columns) {
				Revoke(column.acl, info.grantee_id, grantor, info.privileges & ACL_COLUMN_PRIVILEGES, info.option_only,
				       info.cascade);
			}
		}
	}

	if (!info.column_privileges.empty() && !columns) {
		throw InvalidInputException("column privileges are only valid for relations");
	}
	for (auto &column_privilege : info.column_privileges) {
		auto &column_acl = ColumnRow(*columns, column_privilege.column);
		const auto column_grantable =
		    is_owner ? ACL_COLUMN_PRIVILEGES
		             : (grantable | HeldBy(column_acl, grantor, &AclItem::grant_option)) & ACL_COLUMN_PRIVILEGES;
		Apply(column_acl, grantor, column_grantable, column_privilege.privileges & ACL_COLUMN_PRIVILEGES, info);
	}
	if (columns) {
		columns->erase(std::remove_if(columns->begin(), columns->end(),
		                              [](const ColumnAcl &column) { return column.acl.empty(); }),
		               columns->end());
	}
}

void AlterPermissionsInfo::ApplyTo(Permissions &permissions, CatalogType entry_type,
                                   optional_ptr<vector<ColumnAcl>> columns) const {
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
			                              const auto baseline = row.scope == 0
			                                                        ? Permissions::AclDefault(row.objtype, row.role)
			                                                        : vector<AclItem> {};
			                              return row.role == role ||
			                                     std::is_permutation(row.acl.begin(), row.acl.end(), baseline.begin(),
			                                                         baseline.end());
		                              }),
		               defaults.end());
		return;
	}
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

	if (info.default_objtype != CatalogType::INVALID) {
		auto row = std::find_if(defaults.begin(), defaults.end(), [&](const DefaultAcl &entry) {
			return entry.role == info.target_role && entry.scope == info.default_scope &&
			       entry.objtype == info.default_objtype;
		});
		const auto baseline = info.default_scope == 0 ? Permissions::AclDefault(info.default_objtype, info.target_role)
		                                              : vector<AclItem> {};
		auto row_acl = row == defaults.end() ? baseline : row->acl;
		Apply(row_acl, info.target_role, ObjectKindOf(info.default_objtype).privileges, info.privileges, info);
		if (std::is_permutation(row_acl.begin(), row_acl.end(), baseline.begin(), baseline.end())) {
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

	for (auto &target : info.database_rows) {
		auto &stored = permissions.MutableRows().rows;
		auto range = KindRange(stored, target.kind);
		auto row = std::find_if(range.first, range.second, [&](const GrantRow &candidate) {
			return candidate.Is(target.kind, target.oid, target.name);
		});
		auto row_acl = row != range.second ? row->acl : target.acl;
		ApplyGrant(info, info.object_owner_id, row_acl, target.kind, nullptr);
		const bool restates_default =
		    target.kind == CatalogType::PARAMETER_ENTRY &&
		    (row_acl.empty() || row_acl == Permissions::AclDefault(target.kind, info.object_owner_id));
		if (restates_default && row != range.second) {
			stored.erase(row);
		} else if (row != range.second) {
			row->acl = std::move(row_acl);
		} else if (!restates_default) {
			stored.insert(range.second, GrantRow {target.kind, target.oid, target.name, std::move(row_acl)});
		}
	}
	if (!info.database_rows.empty()) {
		return;
	}

	ApplyGrant(info, owner, acl, ObjectKindOf(entry_type).acl_class, columns);
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

std::span<const AclItem> FindColumnAcl(const vector<ColumnAcl> &columns, const Identifier &column) {
	for (auto &row : columns) {
		if (row.column == column) {
			return row.acl;
		}
	}
	return {};
}

} // namespace duckdb
