//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/exception/parser_exception.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {

class ParserException : public Exception {
public:
	DUCKDB_API explicit ParserException(std::string_view msg);

	DUCKDB_API explicit ParserException(const unordered_map<string, string> &extra_info, std::string_view msg);

	template <typename... ARGS>
	explicit ParserException(std::string_view msg, ARGS &&...params)
	    : ParserException(ConstructMessage(msg, std::forward<ARGS>(params)...)) {
	}
	template <typename... ARGS>
	explicit ParserException(optional_idx error_location, std::string_view msg, ARGS &&...params)
	    : ParserException(Exception::InitializeExtraInfo(error_location),
	                      ConstructMessage(msg, std::forward<ARGS>(params)...)) {
	}
	template <typename... ARGS>
	explicit ParserException(const ParsedExpression &expr, std::string_view msg, ARGS &&...params)
	    : ParserException(Exception::InitializeExtraInfo(expr), ConstructMessage(msg, std::forward<ARGS>(params)...)) {
	}

	static ParserException SyntaxError(std::string_view query, std::string_view error_message,
	                                   optional_idx error_location);
};

} // namespace duckdb
