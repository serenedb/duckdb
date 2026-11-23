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

#include <algorithm>

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

} // namespace duckdb
