#pragma once
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/parsed_expression.hpp"

#include <optional>

namespace duckdb {

struct IndexOpclassDefinition {
	string name;
	std::optional<case_insensitive_map_t<Value>> options;
};

struct IndexElementDefinition {
	unique_ptr<ParsedExpression> expr;
	string opclass;
	std::optional<case_insensitive_map_t<Value>> opclass_options;
};

struct IncludedColumnDefinition {
	string name;
	string opclass;
	std::optional<case_insensitive_map_t<Value>> opclass_options;
};

} // namespace duckdb
