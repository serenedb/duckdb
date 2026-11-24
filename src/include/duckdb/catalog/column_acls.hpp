//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/column_acls.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/role_graph.hpp"
#include "duckdb/parser/column_list.hpp"

#include <algorithm>
#include <span>

namespace duckdb {

template <class F>
bool AnyColumnAcl(const StandardEntry &relation, F &&f) {
	if (relation.type == CatalogType::VIEW_ENTRY) {
		auto &columns = relation.Cast<ViewCatalogEntry>().column_acls;
		return std::any_of(columns.begin(), columns.end(),
		                   [&](const ColumnAcl &column) { return f(std::span<const AclItem>(column.acl)); });
	}
	if (relation.type != CatalogType::TABLE_ENTRY) {
		return false;
	}
	auto &table = relation.Cast<TableCatalogEntry>();
	if (!table.has_column_acls) {
		return false;
	}
	auto columns = table.GetColumns().Logical();
	return std::any_of(columns.begin(), columns.end(),
	                   [&](const ColumnDefinition &column) { return f(std::span<const AclItem>(column.Acl())); });
}

inline std::span<const AclItem> ColumnAclOf(const StandardEntry &relation, idx_t column) {
	if (relation.type == CatalogType::VIEW_ENTRY) {
		return relation.Cast<ViewCatalogEntry>().ColumnAcl(column);
	}
	return relation.Cast<TableCatalogEntry>().GetColumns().GetColumn(LogicalIndex(column)).Acl();
}

inline bool AllowedAny(const RoleAccess &role, CatalogType type, const Permissions &permissions, AclMode privileges,
                       AclMode grant_options, std::span<const std::span<const AclItem>> columns = {},
                       optional_ptr<const ColumnList> all_columns = nullptr) {
	if (privileges == AclMode::NoRights && grant_options == AclMode::NoRights) {
		return role.is_superuser;
	}
	const auto any_column = [&](auto &&visit) {
		for (auto acl : columns) {
			if (visit(acl)) {
				return true;
			}
		}
		if (all_columns) {
			for (auto &column : all_columns->Logical()) {
				if (visit(std::span<const AclItem>(column.Acl()))) {
					return true;
				}
			}
		}
		return false;
	};
	if (privileges != AclMode::NoRights &&
	    (role.CanAny(type, permissions, privileges) || any_column([&](std::span<const AclItem> acl) {
		     return (role.HeldModes(acl) & privileges) != AclMode::NoRights;
	     }))) {
		return true;
	}
	if (grant_options == AclMode::NoRights) {
		return false;
	}
	if (role.Owns(permissions.owner)) {
		return true;
	}
	auto held = role.GrantableModes(permissions.acl);
	any_column([&](std::span<const AclItem> acl) {
		held |= role.GrantableModes(acl);
		return false;
	});
	return (held & grant_options) != AclMode::NoRights;
}

} // namespace duckdb
