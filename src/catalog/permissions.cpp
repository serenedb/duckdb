#include "duckdb/catalog/permissions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/function_entry.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/pragma_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/role_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/window_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/default/default_schemas.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_info.hpp"
#include "duckdb/parser/parsed_data/create_subscription_info.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/storage/storage_manager.hpp"

#include <algorithm>
#include <bit>

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
	const auto allowed = AllPrivileges(acl_class);
	if (StringUtil::CIEquals(keyword, "ALL")) {
		result = allowed;
		return true;
	}
	result = FindPrivilege(keyword);
	return result != AclMode::NoRights && (allowed & result) == result;
}

vector<AclItem> Permissions::AclDefault(CatalogType acl_class, idx_t owner) {
	vector<AclItem> acl;
	const auto owner_privs = AllPrivileges(acl_class);
	if (owner_privs == AclMode::NoRights) {
		return acl;
	}
	const auto public_privs = PublicPrivileges(acl_class);
	if (public_privs != AclMode::NoRights) {
		acl.push_back(AclItem {ACL_ID_PUBLIC, owner, public_privs, AclMode::NoRights});
	}
	acl.push_back(AclItem {owner, owner, owner_privs, AclMode::NoRights});
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
	switch (kind) {
	case CatalogType::PARAMETER_ENTRY:
		return StringUtil::CIEquals(name, name_p);
	case CatalogType::MACRO_ENTRY:
		return name == name_p;
	default:
		return oid == oid_p;
	}
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
	AclMode grantable = Permissions::AllPrivileges(acl_class);
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
		Apply(row_acl, info.target_role, Permissions::AllPrivileges(info.default_objtype), info.privileges, info);
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

	auto &targets = info.database_rows;
	for (idx_t first = 0; first < targets.size();) {
		const auto kind = targets[first].kind;
		auto last = first;
		while (last < targets.size() && targets[last].kind == kind) {
			last++;
		}
		auto &stored = permissions.MutableRows().rows;
		auto range = KindRange(stored, kind);
		vector<GrantRow> rows(std::make_move_iterator(range.first), std::make_move_iterator(range.second));
		auto insert_at = stored.erase(range.first, range.second) - stored.begin();
		unordered_map<string, idx_t> functions;
		if (kind == CatalogType::MACRO_ENTRY) {
			for (idx_t i = 0; i < rows.size(); i++) {
				functions.emplace(rows[i].name, i);
			}
		}
		vector<idx_t> dropped;
		for (auto t = first; t < last; t++) {
			auto &target = targets[t];
			optional_idx position;
			if (kind == CatalogType::MACRO_ENTRY) {
				auto found = functions.find(target.name);
				if (found != functions.end()) {
					position = found->second;
				}
			} else {
				for (idx_t i = 0; i < rows.size() && !position.IsValid(); i++) {
					if (rows[i].Is(target.kind, target.oid, target.name)) {
						position = i;
					}
				}
			}
			auto row_acl = position.IsValid() ? rows[position.GetIndex()].acl : target.acl;
			auto row_columns = position.IsValid() ? rows[position.GetIndex()].columns : vector<ColumnAcl> {};
			ApplyGrant(info, info.object_owner_id, row_acl, target.kind,
			           target.kind == CatalogType::TABLE_ENTRY ? &row_columns : nullptr);
			bool restates_default = false;
			if (target.kind == CatalogType::PARAMETER_ENTRY) {
				restates_default =
				    row_acl.empty() || row_acl == Permissions::AclDefault(target.kind, info.object_owner_id);
			} else if (target.kind == CatalogType::TABLE_ENTRY || target.kind == CatalogType::SCHEMA_ENTRY) {
				restates_default =
				    !target.acl.empty() && row_columns.empty() &&
				    std::is_permutation(row_acl.begin(), row_acl.end(), target.acl.begin(), target.acl.end());
			}
			if (restates_default) {
				if (position.IsValid()) {
					dropped.push_back(position.GetIndex());
				}
			} else if (position.IsValid()) {
				rows[position.GetIndex()].acl = std::move(row_acl);
				rows[position.GetIndex()].columns = std::move(row_columns);
			} else {
				if (target.kind == CatalogType::MACRO_ENTRY) {
					functions.emplace(target.name, rows.size());
				}
				rows.push_back(
				    GrantRow {target.kind, target.oid, target.name, std::move(row_acl), std::move(row_columns)});
			}
		}
		std::sort(dropped.rbegin(), dropped.rend());
		for (auto index : dropped) {
			rows.erase(rows.begin() + NumericCast<int64_t>(index));
		}
		stored.insert(stored.begin() + insert_at, std::make_move_iterator(rows.begin()),
		              std::make_move_iterator(rows.end()));
		first = last;
	}
	if (!targets.empty()) {
		return;
	}

	ApplyGrant(info, owner, acl, Permissions::AclClass(entry_type), columns);
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
	switch (type) {
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
	case CatalogType::SEQUENCE_ENTRY:
		if (access.Has(RoleCapability::ReadAllData)) {
			modes |= AclMode::Select;
		}
		if (access.Has(RoleCapability::WriteAllData)) {
			modes |= AclMode::Insert | AclMode::Update | AclMode::Delete;
		}
		if (access.Has(RoleCapability::Maintain) && type != CatalogType::SEQUENCE_ENTRY) {
			modes |= AclMode::Maintain;
		}
		break;
	case CatalogType::SCHEMA_ENTRY:
		if (access.Has(RoleCapability::ReadAllData) || access.Has(RoleCapability::WriteAllData)) {
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

bool ConstraintKeyReader::CanRead(std::span<const LogicalIndex> columns) const {
	vector<std::span<const AclItem>> acls;
	for (auto column : columns) {
		acls.emplace_back(table.GetColumns().GetColumn(column).Acl());
	}
	return role.CanColumns(table.permissions, AclMode::Select, acls);
}

AclMode RoleAccess::HeldModes(std::span<const AclItem> acl) const {
	return Held(*this, acl, &AclItem::privs);
}

AclMode RoleAccess::GrantableModes(std::span<const AclItem> acl) const {
	return Held(*this, acl, &AclItem::grant_option);
}

static vector<idx_t> Reachable(const unordered_map<idx_t, SecuritySnapshot::Node> &nodes, idx_t role,
                               bool Membership::*option) {
	unordered_set<idx_t> seen {role};
	vector<idx_t> work {role};
	while (!work.empty()) {
		auto it = nodes.find(work.back());
		work.pop_back();
		if (it == nodes.end()) {
			continue;
		}
		for (auto &edge : it->second.member_of) {
			if ((!option || edge.*option) && nodes.count(edge.role) && seen.insert(edge.role).second) {
				work.push_back(edge.role);
			}
		}
	}
	vector<idx_t> result(seen.begin(), seen.end());
	std::sort(result.begin(), result.end());
	return result;
}

SecuritySnapshot::SecuritySnapshot(unordered_map<idx_t, Node> nodes_p, unordered_map<string, idx_t> database_owners_p,
                                   idx_t database_owner_role_p,
                                   unordered_map<string, unordered_map<idx_t, Relation>> relations_p,
                                   unordered_map<string, unordered_map<string, Permissions>> functions_p,
                                   unordered_map<string, unordered_map<idx_t, Permissions>> types_p)
    : nodes(std::move(nodes_p)), database_owners(std::move(database_owners_p)),
      database_owner_role(database_owner_role_p), relations(std::move(relations_p)), functions(std::move(functions_p)),
      types(std::move(types_p)) {
	vector<Role> roles_p;
	roles_p.reserve(nodes.size());
	for (auto &[oid, node] : nodes) {
		by_name.emplace(node.name, oid);
		auto closure = Reachable(nodes, oid, &Membership::inherit_option);
		auto capabilities = RoleCapability::None;
		for (auto member : closure) {
			capabilities |= nodes.find(member)->second.capabilities;
		}
		roles_p.push_back(
		    Role {oid, node.options, capabilities, std::move(closure), Reachable(nodes, oid, &Membership::set_option)});
	}
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

shared_ptr<const SecuritySnapshot> SecuritySnapshot::Build(Catalog &cluster, CatalogTransaction transaction) {
	unordered_map<idx_t, Node> nodes;
	cluster.ScanTopLevelEntries(transaction, CatalogType::ROLE_ENTRY, [&](CatalogEntry &entry) {
		auto &role = entry.Cast<RoleEntry>();
		nodes[role.oid] = Node {role.name.GetIdentifierName(), role.Options(), role.Capabilities(), role.MemberOf()};
	});
	unordered_map<string, idx_t> database_owners;
	unordered_map<string, unordered_map<idx_t, Relation>> relations;
	unordered_map<string, unordered_map<string, Permissions>> functions;
	unordered_map<string, unordered_map<idx_t, Permissions>> types;
	auto &config = DBConfig::GetConfig(cluster.GetDatabase());
	cluster.ScanTopLevelEntries(transaction, CatalogType::DATABASE_ENTRY, [&](CatalogEntry &entry) {
		auto &database = entry.name.GetIdentifierName();
		database_owners.emplace(database, entry.permissions.owner);
		for (auto &row : entry.permissions.Rows().rows) {
			switch (row.kind) {
			case CatalogType::TABLE_ENTRY:
			case CatalogType::SCHEMA_ENTRY:
				relations[database][row.oid] = Relation {Permissions {config.bootstrap_role, row.acl}, row.columns};
				break;
			case CatalogType::MACRO_ENTRY:
				functions[database][row.name] = Permissions {config.bootstrap_role, row.acl};
				break;
			case CatalogType::TYPE_ENTRY:
				types[database][row.oid] = Permissions {config.bootstrap_role, row.acl};
				break;
			default:
				break;
			}
		}
	});
	return make_shared_ptr<const SecuritySnapshot>(std::move(nodes), std::move(database_owners),
	                                               config.database_owner_role, std::move(relations),
	                                               std::move(functions), std::move(types));
}

shared_ptr<const SecuritySnapshot> SecuritySnapshot::ForDatabase(const Identifier &database) const {
	if (!nodes.count(database_owner_role)) {
		return nullptr;
	}
	auto owner = database_owners.find(database.GetIdentifierName());
	if (owner == database_owners.end() || !nodes.count(owner->second)) {
		return nullptr;
	}
	lock_guard<mutex> guard(derived_lock);
	auto &result = derived[owner->second];
	if (!result) {
		auto implied = nodes;
		implied[owner->second].member_of.push_back(
		    Membership {database_owner_role, database_owner_role, false, true, true});
		result = make_shared_ptr<const SecuritySnapshot>(std::move(implied), unordered_map<string, idx_t>(), 0,
		                                                 relations, functions, types);
	}
	return result;
}

optional_ptr<const RoleAccess> SecuritySnapshot::Access(idx_t role) const {
	auto it = std::lower_bound(roles.begin(), roles.end(), role,
	                           [](const RoleAccess &access, idx_t value) { return access.role < value; });
	if (it == roles.end() || it->role != role) {
		return nullptr;
	}
	return &*it;
}

RoleAccess SecuritySnapshot::AccessOf(idx_t role) const {
	auto access = Access(role);
	return access ? *access : RoleAccess {.role = role};
}

optional_ptr<const SecuritySnapshot::Node> SecuritySnapshot::Find(idx_t role) const {
	auto it = nodes.find(role);
	return it == nodes.end() ? nullptr : &it->second;
}

optional_idx SecuritySnapshot::FindByName(std::string_view name) const {
	auto it = by_name.find(name);
	return it == by_name.end() ? optional_idx() : optional_idx(it->second);
}

std::string_view SecuritySnapshot::NameOf(idx_t role) const {
	auto node = Find(role);
	return node ? std::string_view(node->name) : std::string_view();
}

RoleMemberships SecuritySnapshot::MembershipsOf(idx_t role) const {
	RoleMemberships result;
	if (role == 0) {
		return result;
	}
	result.members = Reachable(nodes, role, nullptr);
	for (auto member : result.members) {
		auto node = Find(member);
		if (!node) {
			continue;
		}
		for (auto &edge : node->member_of) {
			if (edge.admin_option) {
				result.admin.push_back(edge.role);
			}
		}
	}
	std::sort(result.admin.begin(), result.admin.end());
	return result;
}

optional_ptr<const SecuritySnapshot::Relation> SecuritySnapshot::FindRelation(std::string_view database,
                                                                              idx_t relation) const {
	if (relations.empty()) {
		return nullptr;
	}
	auto rows = relations.find(database);
	if (rows == relations.end()) {
		return nullptr;
	}
	auto row = rows->second.find(relation);
	return row == rows->second.end() ? nullptr : &row->second;
}

optional_ptr<const Permissions> SecuritySnapshot::RelationPermissions(std::string_view database, idx_t relation) const {
	auto row = FindRelation(database, relation);
	return row ? &row->permissions : nullptr;
}

optional_ptr<const vector<ColumnAcl>> SecuritySnapshot::RelationColumns(std::string_view database,
                                                                        idx_t relation) const {
	auto row = FindRelation(database, relation);
	return row && !row->columns.empty() ? &row->columns : nullptr;
}

optional_ptr<const unordered_map<string, Permissions>>
SecuritySnapshot::FunctionGrants(std::string_view database) const {
	auto rows = functions.find(database);
	return rows == functions.end() ? nullptr : &rows->second;
}

optional_ptr<const Permissions> SecuritySnapshot::TypePermissions(std::string_view database, idx_t type) const {
	auto rows = types.find(database);
	if (rows == types.end()) {
		return nullptr;
	}
	auto row = rows->second.find(type);
	return row == rows->second.end() ? nullptr : &row->second;
}

const Permissions &EffectivePermissions(ClientContext &context, const CatalogEntry &entry) {
	if (!entry.internal || !context.security) {
		return entry.permissions;
	}
	auto permissions =
	    context.security->RelationPermissions(entry.ParentCatalog().GetName().GetIdentifierName(), entry.oid);
	return permissions ? *permissions : entry.permissions;
}

optional_ptr<const vector<ColumnAcl>> EffectiveColumnAcls(ClientContext &context, const CatalogEntry &entry) {
	if (!entry.internal || !context.security) {
		return nullptr;
	}
	return context.security->RelationColumns(entry.ParentCatalog().GetName().GetIdentifierName(), entry.oid);
}

std::span<const AclItem> FindColumnAcl(const vector<ColumnAcl> &columns, const Identifier &column) {
	for (auto &row : columns) {
		if (row.column == column) {
			return row.acl;
		}
	}
	return {};
}

std::span<const AclItem> ColumnAclAt(const CatalogEntry &entry, optional_ptr<const vector<ColumnAcl>> column_acls,
                                     idx_t column_index) {
	if (entry.type == CatalogType::VIEW_ENTRY) {
		auto &view = entry.Cast<ViewCatalogEntry>();
		auto &rows = column_acls ? *column_acls : view.column_acls;
		if (rows.empty()) {
			return {};
		}
		return FindColumnAcl(rows, column_index < view.aliases.size() ? view.aliases[column_index]
		                                                              : view.GetColumnInfo()->names[column_index]);
	}
	auto &column = entry.Cast<TableCatalogEntry>().GetColumn(LogicalIndex(column_index));
	return column_acls ? FindColumnAcl(*column_acls, column.Name()) : std::span<const AclItem>(column.Acl());
}

optional_idx SecuritySnapshot::BestAdmin(idx_t member, idx_t role) const {
	if (member == role) {
		return optional_idx();
	}
	unordered_set<idx_t> seen {member};
	vector<idx_t> level {member};
	while (!level.empty()) {
		vector<idx_t> next;
		for (auto current : level) {
			auto node = Find(current);
			if (!node) {
				continue;
			}
			for (auto &edge : node->member_of) {
				if (edge.role == role && edge.admin_option) {
					return current;
				}
			}
			for (auto &edge : node->member_of) {
				if (edge.inherit_option && seen.insert(edge.role).second) {
					next.push_back(edge.role);
				}
			}
		}
		level = std::move(next);
	}
	return optional_idx();
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

bool CanUseNamedSchema(ClientContext &context, const RoleAccess &role, const CatalogEntry &schema) {
	if (!schema.internal || role.is_superuser) {
		return CanUseSchema(role, schema);
	}
	auto &permissions = EffectivePermissions(context, schema);
	return &permissions == &schema.permissions || role.Can(CatalogType::SCHEMA_ENTRY, permissions, AclMode::Usage);
}

idx_t FunctionOverloadCount(const CatalogEntry &function) {
	switch (function.type) {
	case CatalogType::SCALAR_FUNCTION_ENTRY:
		return function.Cast<ScalarFunctionCatalogEntry>().functions.functions.size();
	case CatalogType::AGGREGATE_FUNCTION_ENTRY:
		return function.Cast<AggregateFunctionCatalogEntry>().functions.functions.size();
	case CatalogType::TABLE_FUNCTION_ENTRY:
		return function.Cast<TableFunctionCatalogEntry>().functions.functions.size();
	case CatalogType::PRAGMA_FUNCTION_ENTRY:
		return function.Cast<PragmaFunctionCatalogEntry>().functions.functions.size();
	case CatalogType::WINDOW_FUNCTION_ENTRY:
		return function.Cast<WindowFunctionCatalogEntry>().functions.functions.size();
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY:
		return function.Cast<MacroCatalogEntry>().macros.size();
	default:
		return 0;
	}
}

vector<AclItem> BuiltinFunctionAcl(const CatalogEntry &function, idx_t owner) {
	if (function.Cast<FunctionEntry>().required_capability == RoleCapability::None) {
		return Permissions::AclDefault(CatalogType::MACRO_ENTRY, owner);
	}
	return {AclItem {owner, owner, AclMode::Execute, AclMode::NoRights}};
}

vector<GrantRow> BuiltinFunctionRows(ClientContext &context, SchemaCatalogEntry &schema) {
	auto &config = DBConfig::GetConfig(context);
	vector<GrantRow> rows;
	unordered_set<string> keys;
	const auto add = [&](CatalogEntry &entry) {
		if (!entry.internal) {
			return;
		}
		const auto overloads = FunctionOverloadCount(entry);
		for (idx_t overload = 0; overload < overloads; overload++) {
			auto key = config.builtin_function_key(entry, overload);
			if (!key.empty() && keys.insert(key).second) {
				rows.push_back(GrantRow {
				    CatalogType::MACRO_ENTRY, 0, std::move(key), BuiltinFunctionAcl(entry, config.bootstrap_role), {}});
			}
		}
	};
	for (auto kind : {CatalogType::MACRO_ENTRY, CatalogType::TABLE_MACRO_ENTRY}) {
		schema.Scan(context, kind, add);
	}
	const Identifier information_schema("information_schema");
	const bool information = schema.name == information_schema;
	Catalog::GetSystemCatalog(context).ScanSchemas(context, [&](SchemaCatalogEntry &system_schema) {
		if ((system_schema.name == information_schema) != information) {
			return;
		}
		for (auto kind : {CatalogType::SCALAR_FUNCTION_ENTRY, CatalogType::TABLE_FUNCTION_ENTRY,
		                  CatalogType::PRAGMA_FUNCTION_ENTRY}) {
			system_schema.Scan(context, kind, add);
		}
	});
	return rows;
}

optional_ptr<const unordered_map<string, Permissions>> BuiltinFunctionGrants(ClientContext &context) {
	if (!context.security || !context.security->HasFunctionGrants()) {
		return nullptr;
	}
	return context.security->FunctionGrants(DatabaseManager::TryGetDefaultDatabase(context).GetIdentifierName());
}

optional_ptr<const Permissions> BuiltinTypeGrant(ClientContext &context, idx_t type) {
	if (!context.security || !context.security->HasTypeGrants()) {
		return nullptr;
	}
	return context.security->TypePermissions(DatabaseManager::TryGetDefaultDatabase(context).GetIdentifierName(), type);
}

static optional_ptr<const Permissions> GrantRow(optional_ptr<const unordered_map<string, Permissions>> grants,
                                                std::string_view key) {
	if (!grants) {
		return nullptr;
	}
	auto row = grants->find(key);
	return row == grants->end() ? nullptr : &row->second;
}

static bool MayCallBuiltin(ClientContext &context, const RoleAccess &role, optional_ptr<const Permissions> granted,
                           RoleCapability capability) {
	if (capability != RoleCapability::None && role.Has(capability)) {
		return true;
	}
	if (granted) {
		return role.Can(CatalogType::MACRO_ENTRY, *granted, AclMode::Execute);
	}
	return capability == RoleCapability::None || role.Owns(DBConfig::GetConfig(context).bootstrap_role);
}

static optional_ptr<const Permissions> BuiltinGrant(ClientContext &context, const CatalogEntry &function,
                                                    idx_t overload) {
	auto &config = DBConfig::GetConfig(context);
	if (!config.builtin_function_key || !function.internal) {
		return nullptr;
	}
	auto grants = BuiltinFunctionGrants(context);
	return grants ? GrantRow(grants, config.builtin_function_key(function, overload)) : nullptr;
}

void CheckBuiltinCall(ClientContext &context, const RoleAccess &role, const CatalogEntry &function, idx_t overload) {
	if (!role.is_superuser && !MayCallBuiltin(context, role, BuiltinGrant(context, function, overload),
	                                          function.Cast<FunctionEntry>().required_capability)) {
		ThrowPermissionDenied(function);
	}
}

bool MayCallFunction(ClientContext &context, const RoleAccess &role, const CatalogEntry &function) {
	const auto capability = function.Cast<FunctionEntry>().required_capability;
	if (capability == RoleCapability::None) {
		return true;
	}
	const auto overloads = FunctionOverloadCount(function);
	for (idx_t overload = 0; overload < overloads; overload++) {
		if (MayCallBuiltin(context, role, BuiltinGrant(context, function, overload), capability)) {
			return true;
		}
	}
	return false;
}

static optional_ptr<Catalog> RoleCatalog(ClientContext &context) {
	auto &system = Catalog::GetSystemCatalog(context);
	if (system.IsDuckCatalog()) {
		return nullptr;
	}
	return &system;
}

static optional_ptr<CatalogEntry> ClusterDatabaseOf(ClientContext &context, AttachedDatabase &database) {
	return database.IsReadOnly() ? nullptr : ClusterEntry(context, CatalogType::DATABASE_ENTRY, database.GetName());
}

static optional_ptr<CatalogEntry> FindForeignServer(ClientContext &context, const Identifier &name) {
	for (auto &db : DatabaseManager::Get(*context.db).GetDatabases()) {
		if (db->IsSystem() || db->IsTemporary() || db->IsReadOnly()) {
			continue;
		}
		auto &catalog = db->GetCatalog();
		if (auto server = catalog.LookupTopLevelEntry(catalog.GetCatalogTransaction(context),
		                                              CatalogType::FOREIGN_SERVER_ENTRY, name)) {
			return server;
		}
	}
	return nullptr;
}

static bool CanConnect(ClientContext &context, const RoleAccess &role, const Identifier &name,
                       optional_ptr<CatalogEntry> database) {
	return !database || name.GetIdentifierName() == ClientData::Get(context).default_database.GetIdentifierName() ||
	       role.Can(CatalogType::DATABASE_ENTRY, database->permissions, AclMode::Connect);
}

bool DatabaseVisible(ClientContext &context, const RoleAccess &role, AttachedDatabase &database) {
	if (database.IsSystem() || database.IsTemporary()) {
		return true;
	}
	if (auto entry = ClusterDatabaseOf(context, database)) {
		return CanConnect(context, role, database.GetName(), entry);
	}
	auto server = FindForeignServer(context, database.GetName());
	return server && role.Can(CatalogType::FOREIGN_SERVER_ENTRY, server->permissions, AclMode::Usage);
}

bool EntryVisible(ClientContext &context, const RoleAccess &role, CatalogEntry &entry) {
	if (entry.type == CatalogType::DATABASE_ENTRY) {
		return CanConnect(context, role, entry.name, ClusterEntry(context, CatalogType::DATABASE_ENTRY, entry.name));
	}
	if ((entry.type == CatalogType::TABLE_FUNCTION_ENTRY || entry.type == CatalogType::SCALAR_FUNCTION_ENTRY) &&
	    !MayCallFunction(context, role, entry)) {
		return false;
	}
	if (entry.internal || IsUnowned(entry)) {
		return true;
	}
	auto &permissions = entry.permissions;
	switch (entry.type) {
	case CatalogType::SCHEMA_ENTRY:
		return role.CanAny(entry.type, permissions, AclMode::Usage | AclMode::Create);
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY: {
		if (role.CanAny(CatalogType::TABLE_ENTRY, permissions, Permissions::AllPrivileges(CatalogType::TABLE_ENTRY))) {
			return true;
		}
		const auto grants = [&](std::span<const AclItem> acl) {
			return (role.HeldModes(acl) & ACL_COLUMN_PRIVILEGES) != AclMode::NoRights;
		};
		if (entry.type == CatalogType::VIEW_ENTRY) {
			auto &column_acls = entry.Cast<ViewCatalogEntry>().column_acls;
			return std::any_of(column_acls.begin(), column_acls.end(),
			                   [&](const ColumnAcl &column) { return grants(column.acl); });
		}
		auto columns = entry.Cast<TableCatalogEntry>().GetColumns().Logical();
		return std::any_of(columns.begin(), columns.end(),
		                   [&](const ColumnDefinition &column) { return grants(column.Acl()); });
	}
	case CatalogType::SEQUENCE_ENTRY:
		return role.CanAny(entry.type, permissions, AclMode::Usage | AclMode::Select | AclMode::Update);
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY: {
		auto &macro = entry.Cast<MacroCatalogEntry>();
		return std::any_of(macro.macros.begin(), macro.macros.end(), [&](const unique_ptr<MacroFunction> &overload) {
			return role.Can(entry.type, macro.OverloadPermissions(*overload), AclMode::Execute);
		});
	}
	case CatalogType::TYPE_ENTRY:
		return role.Can(entry.type, permissions, AclMode::Usage);
	case CatalogType::JOB_ENTRY:
		return role.Owns(permissions.owner);
	case CatalogType::INDEX_ENTRY: {
		auto &index = entry.Cast<IndexCatalogEntry>();
		auto relation = index.GetRelation(index.ParentCatalog().GetCatalogTransaction(context));
		return !relation || EntryVisible(context, role, *relation);
	}
	default:
		return true;
	}
}

void CheckReference(ClientContext &context, const RoleAccess &role, CatalogEntry &entry) {
	auto &database = entry.type == CatalogType::DATABASE_ENTRY ? entry.Cast<AttachedDatabase>()
	                                                           : entry.ParentCatalog().GetAttached();
	if (!DatabaseVisible(context, role, database)) {
		auto &name = database.GetName().GetIdentifierName();
		if (ClusterDatabaseOf(context, database)) {
			throw PermissionException({{"detail", "User does not have CONNECT privilege."}},
			                          StringUtil::Format("permission denied for database \"%s\"", name));
		}
		if (auto server = FindForeignServer(context, database.GetName())) {
			ThrowPermissionDenied(*server);
		}
		throw PermissionException(
		    {{"detail",
		      "Only roles with the SUPERUSER attribute may use a database attached without a foreign server."}},
		    StringUtil::Format("permission denied for database \"%s\"", name));
	}
	if (entry.type != CatalogType::DATABASE_ENTRY && !EntryVisible(context, role, entry)) {
		ThrowPermissionDenied(entry);
	}
}

template <class F>
static bool AnyRoleReference(const CatalogEntry &entry, F &&named) {
	auto &permissions = entry.permissions;
	const auto any_acl = [&](const vector<AclItem> &acl) {
		return std::any_of(acl.begin(), acl.end(),
		                   [&](const AclItem &item) { return named(item.grantee) || named(item.grantor); });
	};
	if (entry.type == CatalogType::MACRO_ENTRY || entry.type == CatalogType::TABLE_MACRO_ENTRY) {
		auto &macro = entry.Cast<MacroCatalogEntry>();
		return std::any_of(macro.macros.begin(), macro.macros.end(), [&](const unique_ptr<MacroFunction> &overload) {
			auto &overload_permissions = macro.OverloadPermissions(*overload);
			return named(overload_permissions.owner) || any_acl(overload_permissions.acl);
		});
	}
	if (named(permissions.owner) || any_acl(permissions.acl)) {
		return true;
	}
	for (auto &row : permissions.defaults) {
		if (named(row.role) || any_acl(row.acl)) {
			return true;
		}
	}
	for (auto &row : permissions.Rows().rows) {
		if (any_acl(row.acl) || std::any_of(row.columns.begin(), row.columns.end(),
		                                    [&](const ColumnAcl &column) { return any_acl(column.acl); })) {
			return true;
		}
	}
	if (entry.type == CatalogType::ROLE_ENTRY) {
		auto &member_of = entry.Cast<RoleEntry>().MemberOf();
		return std::any_of(member_of.begin(), member_of.end(),
		                   [&](const Membership &edge) { return named(edge.grantor); });
	}
	if (entry.type == CatalogType::VIEW_ENTRY) {
		auto &column_acls = entry.Cast<ViewCatalogEntry>().column_acls;
		return std::any_of(column_acls.begin(), column_acls.end(),
		                   [&](const ColumnAcl &column) { return any_acl(column.acl); });
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

string RoleOptionsToSQL(RoleOption set_options, RoleOption clear_options) {
	string result;
	for (auto &named : ROLE_OPTIONS) {
		if (HasOption(set_options, named.option)) {
			result += " ";
			result += named.keyword;
		} else if (HasOption(clear_options, named.option)) {
			result += " NO";
			result += named.keyword;
		}
	}
	return result;
}

string ValidUntilToSQL(int64_t valid_until) {
	return KeywordHelper::WriteQuoted(Value::TIMESTAMPTZ(timestamp_tz_t(valid_until)).ToString(), '\'');
}

void ThrowPermissionDenied(CatalogType type, std::string_view name) {
	throw PermissionException("permission denied for %s %s", StringUtil::Lower(CatalogTypeToString(type)), name);
}

void ThrowPermissionDenied(const CatalogEntry &entry) {
	ThrowPermissionDenied(entry.type, entry.name.GetIdentifierName());
}

void RequireEntryPrivilege(const RoleAccess &role, const CatalogEntry &entry, AclMode any) {
	if (!role.CanAny(Permissions::AclClass(entry.type), entry.permissions, any)) {
		ThrowPermissionDenied(entry);
	}
}

void RequireSuperuser(const RoleAccess &role) {
	if (!role.is_superuser) {
		throw PermissionException("permission denied: superuser required");
	}
}

void RequireOwner(const RoleAccess &role, const CatalogEntry &entry, idx_t owner) {
	if (!role.Owns(owner)) {
		throw PermissionException("must be owner of %s %s", StringUtil::Lower(CatalogTypeToString(entry.type)),
		                          entry.name.GetIdentifierName());
	}
}

bool IsAdminOf(ClientContext &context, const RoleAccess &role, idx_t target) {
	return context.security && context.security->MembershipsOf(role.role).IsAdminOf(target);
}

static void RequireRoleOwner(ClientContext &context, const RoleAccess &role, const RoleEntry &target) {
	if (role.is_superuser) {
		return;
	}
	string detail;
	if (target.IsSuperuser()) {
		detail = "The current user must have the SUPERUSER attribute.";
	} else if (!role.Has(RoleOption::CreateRole)) {
		detail = "The current user must have the CREATEROLE attribute.";
	} else if (!IsAdminOf(context, role, target.oid)) {
		detail = StringUtil::Format("The current user must have the ADMIN option on role \"%s\".",
		                            target.name.GetIdentifierName());
	} else {
		return;
	}
	throw PermissionException({{"detail", detail}}, "permission denied");
}

void RequireOwner(ClientContext &context, const RoleAccess &role, CatalogEntry &entry,
                  optional_ptr<MacroFunction> overload) {
	if (entry.type == CatalogType::ROLE_ENTRY) {
		RequireRoleOwner(context, role, entry.Cast<RoleEntry>());
		return;
	}
	if (entry.type == CatalogType::MACRO_ENTRY || entry.type == CatalogType::TABLE_MACRO_ENTRY) {
		auto &macro = entry.Cast<MacroCatalogEntry>();
		if (overload) {
			RequireOwner(role, entry, macro.OverloadPermissions(*overload).owner);
			return;
		}
		for (auto &function : macro.macros) {
			RequireOwner(role, entry, macro.OverloadPermissions(*function).owner);
		}
		return;
	}
	auto owner = entry.permissions.owner;
	if (entry.type == CatalogType::INDEX_ENTRY) {
		auto relation =
		    entry.Cast<IndexCatalogEntry>().GetRelation(entry.ParentCatalog().GetCatalogTransaction(context));
		if (!relation) {
			return;
		}
		owner = relation->permissions.owner;
	}
	RequireOwner(role, entry, owner);
}

void RequireUse(ClientContext &context, const RoleAccess &role, CatalogEntry &entry, EntryUse use,
                optional_ptr<MacroFunction> overload) {
	switch (use) {
	case EntryUse::REFERENCE:
		return;
	case EntryUse::OWN:
		RequireOwner(context, role, entry, overload);
		if (entry.type == CatalogType::SUBSCRIPTION_ENTRY && !role.is_superuser &&
		    !entry.GetInfo()->Cast<CreateSubscriptionInfo>().password_required) {
			throw PermissionException(
			    unordered_map<string, string> {{"hint", "Subscriptions with the password_required option set to false "
			                                            "may only be created or modified by the superuser."}},
			    "password_required=false is superuser-only");
		}
		return;
	case EntryUse::RENAME:
		RequireOwner(context, role, entry, overload);
		if (entry.type == CatalogType::SCHEMA_ENTRY) {
			RequireDatabasePrivilege(context, role, entry.ParentCatalog().GetName(), AclMode::Create);
		} else {
			RequireSchemaCreate(context, role, entry.ParentSchema(context));
		}
		return;
	case EntryUse::DROP:
		if (entry.type == CatalogType::SCHEMA_ENTRY || entry.type == CatalogType::DATABASE_ENTRY ||
		    entry.type == CatalogType::FOREIGN_SERVER_ENTRY || entry.type == CatalogType::SUBSCRIPTION_ENTRY) {
			RequireOwner(context, role, entry);
		} else if (!role.Owns(entry.ParentSchema(context).permissions.owner)) {
			RequireOwner(context, role, entry, overload);
		}
		return;
	}
}

void RequireCreate(ClientContext &context, const RoleAccess &role, SchemaCatalogEntry &schema, const CreateInfo &info,
                   const Identifier &name) {
	RequireSchemaCreate(context, role, schema);
	if (role.is_superuser || (info.on_conflict != OnCreateConflict::REPLACE_ON_CONFLICT &&
	                          info.on_conflict != OnCreateConflict::ALTER_ON_CONFLICT)) {
		return;
	}
	const bool merged_overloads =
	    info.on_conflict == OnCreateConflict::ALTER_ON_CONFLICT &&
	    (info.type == CatalogType::MACRO_ENTRY || info.type == CatalogType::TABLE_MACRO_ENTRY);
	if (merged_overloads) {
		return;
	}
	if (auto existing = schema.GetEntry(schema.catalog.GetCatalogTransaction(context), info.type, name)) {
		RequireOwner(context, role, *existing);
	}
}

optional_ptr<CatalogEntry> ClusterEntry(ClientContext &context, CatalogType type, const Identifier &name) {
	auto cluster = RoleCatalog(context);
	if (!cluster) {
		return nullptr;
	}
	return cluster->LookupTopLevelEntry(cluster->GetCatalogTransaction(context), type, name);
}

optional_ptr<CatalogEntry> TopLevelEntry(ClientContext &context, const RoleAccess &role, Catalog &catalog,
                                         CatalogType type, const Identifier &name, EntryUse use,
                                         OnEntryNotFound if_not_found) {
	auto entry = catalog.LookupTopLevelEntry(catalog.GetCatalogTransaction(context), type, name);
	if (!entry) {
		if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw CatalogException::MissingObject(type, name);
		}
		return nullptr;
	}
	RequireUse(context, role, *entry, use);
	return entry;
}

optional_ptr<CatalogEntry> ClusterEntry(ClientContext &context, const RoleAccess &role, CatalogType type,
                                        const Identifier &name, EntryUse use, OnEntryNotFound if_not_found) {
	auto cluster = RoleCatalog(context);
	if (cluster) {
		return TopLevelEntry(context, role, *cluster, type, name, use, if_not_found);
	}
	if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
		throw CatalogException::MissingObject(type, name);
	}
	return nullptr;
}

CatalogEntry &ClusterDatabase(ClientContext &context, const Identifier &name) {
	return *ClusterEntry(context, RoleAccess::System(), CatalogType::DATABASE_ENTRY, name, EntryUse::REFERENCE,
	                     OnEntryNotFound::THROW_EXCEPTION);
}

QualifiedName ClusterName(ClientContext &context, const QualifiedName &name) {
	auto cluster = RoleCatalog(context);
	if (!cluster) {
		return name;
	}
	return QualifiedName(cluster->GetName(), Identifier(), name.Name());
}

idx_t RoleId(ClientContext &context, const Identifier &name) {
	return ClusterEntry(context, RoleAccess::System(), CatalogType::ROLE_ENTRY, name, EntryUse::REFERENCE,
	                    OnEntryNotFound::THROW_EXCEPTION)
	    ->oid;
}

optional_idx RoleSpecKeyword(ClientContext &context, std::string_view name) {
	if (name == "CURRENT_USER" || name == "CURRENT_ROLE") {
		return context.current_role_id;
	}
	if (name == "SESSION_USER") {
		return context.session_role;
	}
	return optional_idx();
}

idx_t RoleSpecId(ClientContext &context, std::string_view name) {
	const auto keyword = RoleSpecKeyword(context, name);
	return keyword.IsValid() ? keyword.GetIndex() : RoleId(context, Identifier(name));
}

string RoleName(ClientContext &context, idx_t role) {
	auto cluster = RoleCatalog(context);
	if (!cluster) {
		return string();
	}
	auto entry = cluster->LookupOwner(cluster->GetCatalogTransaction(context), role);
	return entry && entry->type == CatalogType::ROLE_ENTRY ? entry->name.GetIdentifierName() : string();
}

static bool StoresPermissions(Catalog &catalog) {
	return !catalog.IsDuckCatalog() || catalog.GetAttached().IsTemporary() ||
	       IsSereneDBStorageVersion(catalog.GetAttached().GetStorageManager().GetStorageVersion());
}

void AssignOwner(ClientContext &context, Permissions &permissions, Catalog &catalog, CatalogType objtype,
                 optional_ptr<SchemaCatalogEntry> schema, idx_t owner) {
	if (!StoresPermissions(catalog)) {
		return;
	}
	auto database = ClusterEntry(context, CatalogType::DATABASE_ENTRY, catalog.GetName());
	permissions.AssignOwner(owner, objtype, database ? &database->permissions : nullptr,
	                        schema ? optional_idx(schema->oid) : optional_idx());
}

void RequireDatabasePrivilege(ClientContext &context, const RoleAccess &role, const Identifier &database,
                              AclMode need) {
	if (role.is_superuser) {
		return;
	}
	if (auto entry = ClusterEntry(context, CatalogType::DATABASE_ENTRY, database)) {
		RequireEntryPrivilege(role, *entry, need);
	}
}

void RequireForeignDataWrapperUsage(ClientContext &context, const RoleAccess &role, const Identifier &database,
                                    std::string_view wrapper) {
	auto &found = DBConfig::GetConfig(context).GetForeignDataWrapper(wrapper);
	if (role.Owns(found.owner)) {
		return;
	}
	auto entry = ClusterEntry(context, CatalogType::DATABASE_ENTRY, database);
	if (!entry || !Allows(role, entry->permissions.RowAcl(CatalogType::FOREIGN_DATA_WRAPPER_ENTRY, found.oid),
	                      CatalogType::FOREIGN_DATA_WRAPPER_ENTRY, AclMode::Usage, false)) {
		ThrowPermissionDenied(CatalogType::FOREIGN_DATA_WRAPPER_ENTRY, wrapper);
	}
}

void RequireLanguageUsage(ClientContext &context, const RoleAccess &role, const Identifier &database,
                          std::string_view language) {
	if (role.is_superuser) {
		return;
	}
	auto &found = DBConfig::GetConfig(context).GetLanguage(language);
	auto entry = ClusterEntry(context, CatalogType::DATABASE_ENTRY, database);
	auto acl = entry ? entry->permissions.RowAcl(CatalogType::LANGUAGE_ENTRY, found.oid) : std::span<const AclItem>();
	if (!found.trusted ||
	    (!role.Owns(found.owner) && !Allows(role, acl, CatalogType::LANGUAGE_ENTRY, AclMode::Usage, false))) {
		ThrowPermissionDenied(CatalogType::LANGUAGE_ENTRY, language);
	}
}

void RequireUserSchema(const SchemaCatalogEntry &schema, const Identifier &name) {
	if (schema.internal && (!schema.catalog.IsDuckCatalog() || DefaultSchemaGenerator::IsDefaultSchema(schema.name))) {
		throw PermissionException(
		    unordered_map<string, string> {{"detail", "System catalog modifications are currently disallowed."}},
		    StringUtil::Format("permission denied to create \"%s.%s\"", schema.name.GetIdentifierName(),
		                       name.GetIdentifierName()));
	}
}

void RequireSchemaCreate(ClientContext &context, const RoleAccess &role, SchemaCatalogEntry &schema) {
	if (role.is_superuser) {
		return;
	}
	if (schema.ParentCatalog().GetAttached().IsTemporary()) {
		auto &database = ClientData::Get(context).default_database;
		auto entry = ClusterEntry(context, CatalogType::DATABASE_ENTRY, database);
		if (entry && !role.Can(CatalogType::DATABASE_ENTRY, entry->permissions, AclMode::CreateTemp)) {
			throw PermissionException("permission denied to create temporary tables in database \"%s\"",
			                          database.GetIdentifierName());
		}
		return;
	}
	RequireEntryPrivilege(role, schema, AclMode::Create);
}

void RequireTypeUsage(ClientContext &context, const RoleAccess &role, const LogicalType &type, Catalog &catalog) {
	if (role.is_superuser) {
		return;
	}
	reference<const LogicalType> element = type;
	while (element.get().id() == LogicalTypeId::LIST || element.get().id() == LogicalTypeId::ARRAY) {
		element = element.get().id() == LogicalTypeId::LIST ? ListType::GetChildType(element)
		                                                    : ArrayType::GetChildType(element);
	}
	if (auto extension = element.get().GetExtensionInfo()) {
		auto oid = extension->properties.find(ExtensionTypeInfo::CATALOG_OID_PROPERTY);
		if (oid != extension->properties.end()) {
			auto entry = catalog.LookupOwner(catalog.GetCatalogTransaction(context), oid->second.GetValue<idx_t>());
			if (entry && entry->type == CatalogType::TYPE_ENTRY && !IsUnowned(*entry)) {
				RequireEntryPrivilege(role, *entry, AclMode::Usage);
			}
			return;
		}
	}
	auto &config = DBConfig::GetConfig(context);
	if (!context.security || !context.security->HasTypeGrants() || !config.builtin_type_oid) {
		return;
	}
	string name;
	const auto oid = config.builtin_type_oid(element, name);
	if (!oid) {
		return;
	}
	auto granted = BuiltinTypeGrant(context, oid);
	if (granted && !role.Can(CatalogType::TYPE_ENTRY, *granted, AclMode::Usage)) {
		ThrowPermissionDenied(CatalogType::TYPE_ENTRY, name);
	}
}

void CheckMacroCall(ClientContext &context, const RoleAccess &role, MacroCatalogEntry &macro,
                    const MacroFunction &overload, idx_t overload_index) {
	if (IsUnowned(macro)) {
		CheckBuiltinCall(context, role, macro, overload_index);
	} else if (!role.Can(macro.type, macro.OverloadPermissions(overload), AclMode::Execute)) {
		ThrowPermissionDenied(macro);
	}
}

FunctionCall FunctionCallOf(ClientContext &context, const CatalogEntry &function, idx_t overload) {
	FunctionCall call;
	call.type = function.type;
	call.name = function.name.GetIdentifierName();
	auto &config = DBConfig::GetConfig(context);
	if (config.builtin_function_key && function.internal) {
		call.builtin_key = config.builtin_function_key(function, overload);
	}
	call.capability = function.Cast<FunctionEntry>().required_capability;
	return call;
}

FunctionCall MacroCallOf(ClientContext &context, MacroCatalogEntry &macro, const MacroFunction &overload,
                         idx_t overload_index) {
	if (IsUnowned(macro)) {
		return FunctionCallOf(context, macro, overload_index);
	}
	FunctionCall call;
	call.type = macro.type;
	call.name = macro.name.GetIdentifierName();
	call.catalog = macro.ParentCatalog().GetName();
	call.macro_overload = macro.OverloadOid(overload);
	return call;
}

void CheckFunctionCalls(ClientContext &context, const RoleAccess &role, std::span<const FunctionCall> calls) {
	if (role.is_superuser) {
		return;
	}
	for (auto &call : calls) {
		if (!call.macro_overload.IsValid()) {
			auto granted =
			    call.builtin_key.empty() ? nullptr : GrantRow(BuiltinFunctionGrants(context), call.builtin_key);
			if (!MayCallBuiltin(context, role, granted, call.capability)) {
				ThrowPermissionDenied(call.type, call.name);
			}
			continue;
		}
		auto catalog = Catalog::GetCatalogEntry(context, call.catalog);
		if (!catalog) {
			continue;
		}
		auto entry = catalog->LookupOwner(catalog->GetCatalogTransaction(context), call.macro_overload.GetIndex());
		if (!entry || (entry->type != CatalogType::MACRO_ENTRY && entry->type != CatalogType::TABLE_MACRO_ENTRY)) {
			continue;
		}
		auto &macro = entry->Cast<MacroCatalogEntry>();
		auto overload = macro.FindOverload(call.macro_overload.GetIndex());
		if (overload && !role.Can(macro.type, macro.OverloadPermissions(*overload), AclMode::Execute)) {
			ThrowPermissionDenied(macro);
		}
	}
}

} // namespace duckdb
