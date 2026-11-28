//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/column_acls.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/acl.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/parser/column_list.hpp"

#include <absl/algorithm/container.h>
#include <span>

namespace duckdb {

template <class F>
bool AnyColumnAcl(const StandardEntry &relation, F &&f) {
	if (relation.type == CatalogType::VIEW_ENTRY) {
		return absl::c_any_of(relation.Cast<ViewCatalogEntry>().column_acls,
		                      [&](const ColumnAcl &column) { return f(std::span<const AclItem>(column.acl)); });
	}
	if (relation.type != CatalogType::TABLE_ENTRY) {
		return false;
	}
	auto &table = relation.Cast<TableCatalogEntry>();
	if (!table.has_column_acls) {
		return false;
	}
	return absl::c_any_of(table.GetColumns().Logical(),
	                      [&](const ColumnDefinition &column) { return f(std::span<const AclItem>(column.Acl())); });
}

inline std::span<const AclItem> ColumnAclOf(const StandardEntry &relation, LogicalIndex column) {
	if (relation.type == CatalogType::VIEW_ENTRY) {
		return relation.Cast<ViewCatalogEntry>().ColumnAcl(column.index);
	}
	return relation.Cast<TableCatalogEntry>().GetColumns().GetColumn(column).Acl();
}

} // namespace duckdb
