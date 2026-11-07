//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/parse_tree_delete.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/winapi.hpp"

#include <memory>

namespace duckdb {

class ParsedExpression;
class QueryNode;
class TableRef;

DUCKDB_API void DeleteParseTree(ParsedExpression *node) noexcept;
DUCKDB_API void DeleteParseTree(QueryNode *node) noexcept;
DUCKDB_API void DeleteParseTree(TableRef *node) noexcept;

template <class NODE>
struct ParseTreeDelete {
	constexpr ParseTreeDelete() noexcept = default;

	template <class DERIVED>
	ParseTreeDelete(const std::default_delete<DERIVED> &) noexcept {
	}

	void operator()(NODE *node) const noexcept {
		DeleteParseTree(node);
	}
};

} // namespace duckdb

template <>
struct std::default_delete<duckdb::ParsedExpression> : duckdb::ParseTreeDelete<duckdb::ParsedExpression> {
	using ParseTreeDelete::ParseTreeDelete;
};

template <>
struct std::default_delete<duckdb::QueryNode> : duckdb::ParseTreeDelete<duckdb::QueryNode> {
	using ParseTreeDelete::ParseTreeDelete;
};

template <>
struct std::default_delete<duckdb::TableRef> : duckdb::ParseTreeDelete<duckdb::TableRef> {
	using ParseTreeDelete::ParseTreeDelete;
};
