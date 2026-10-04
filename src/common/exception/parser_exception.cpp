#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/common/query_location.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/parser/query_error_context.hpp"

namespace duckdb {

ParserException::ParserException(std::string_view msg) : Exception(ExceptionType::PARSER, msg) {
}

ParserException::ParserException(const unordered_map<string, string> &extra_info, std::string_view msg)
    : Exception(extra_info, ExceptionType::PARSER, msg) {
}

ParserException ParserException::SyntaxError(std::string_view query, std::string_view error_message,
                                             QueryLocation error_location) {
	return ParserException(Exception::InitializeExtraInfo("SYNTAX_ERROR", error_location), error_message);
}

void ParserException::ThrowMaxExpressionDepth(idx_t max_expression_depth) {
	throw ParserException("Max expression depth limit of %lld exceeded. Use \"SET max_expression_depth TO x\" to "
	                      "increase the maximum expression depth.",
	                      max_expression_depth);
}
} // namespace duckdb
