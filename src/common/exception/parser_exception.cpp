#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/parser/query_error_context.hpp"

namespace duckdb {

ParserException::ParserException(std::string_view msg) : Exception(ExceptionType::PARSER, msg) {
}

ParserException::ParserException(const unordered_map<string, string> &extra_info, std::string_view msg)
    : Exception(extra_info, ExceptionType::PARSER, msg) {
}

ParserException ParserException::SyntaxError(std::string_view query, std::string_view error_message,
                                             optional_idx error_location) {
	return ParserException(Exception::InitializeExtraInfo("SYNTAX_ERROR", error_location), error_message);
}
} // namespace duckdb
