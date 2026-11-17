#include "duckdb/catalog/role_graph.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/role_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/main/config.hpp"

#include <algorithm>
#include <bit>

namespace duckdb {

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
		return done(have | ObjectKindOf(type).public_privileges);
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

bool RoleAccess::Can(CatalogType type, std::span<const AclItem> acl, AclMode need) const {
	return Allows(*this, acl, type, need, false);
}

bool RoleAccess::Can(CatalogType type, const Permissions &permissions, AclMode need) const {
	return Owns(permissions.owner) || Can(type, permissions.acl, need);
}

bool RoleAccess::CanAny(CatalogType type, const Permissions &permissions, AclMode need) const {
	return Owns(permissions.owner) || Allows(*this, permissions.acl, type, need, true);
}

bool RoleAccess::CanColumns(const StandardEntry &relation, AclMode need, std::span<const idx_t> columns) const {
	if (Can(CatalogType::TABLE_ENTRY, relation.permissions, need)) {
		return true;
	}
	const auto granted = [&](std::span<const AclItem> acl) {
		return Can(CatalogType::TABLE_ENTRY, acl, need);
	};
	if (relation.type == CatalogType::VIEW_ENTRY) {
		auto &view = relation.Cast<ViewCatalogEntry>();
		if (columns.empty()) {
			return std::any_of(view.column_acls.begin(), view.column_acls.end(),
			                   [&](const ColumnAcl &column) { return granted(column.acl); });
		}
		return std::all_of(columns.begin(), columns.end(),
		                   [&](idx_t column) { return granted(view.ColumnAcl(column)); });
	}
	auto &table_columns = relation.Cast<TableCatalogEntry>().GetColumns();
	if (columns.empty()) {
		for (auto &column : table_columns.Logical()) {
			if (granted(column.Acl())) {
				return true;
			}
		}
		return false;
	}
	return std::all_of(columns.begin(), columns.end(),
	                   [&](idx_t column) { return granted(table_columns.GetColumn(LogicalIndex(column)).Acl()); });
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
                                   idx_t database_owner_role_p)
    : nodes(std::move(nodes_p)), database_owners(std::move(database_owners_p)),
      database_owner_role(database_owner_role_p) {
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
	cluster.ScanTopLevelEntries(transaction, CatalogType::DATABASE_ENTRY, [&](CatalogEntry &entry) {
		database_owners.emplace(entry.name.GetIdentifierName(), entry.permissions.owner);
	});
	return make_shared_ptr<const SecuritySnapshot>(std::move(nodes), std::move(database_owners),
	                                               DBConfig::GetConfig(cluster.GetDatabase()).database_owner_role);
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
		result = make_shared_ptr<const SecuritySnapshot>(std::move(implied), unordered_map<string, idx_t>(), 0);
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

} // namespace duckdb
