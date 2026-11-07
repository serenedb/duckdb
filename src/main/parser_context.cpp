#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

Parser::Parser(ClientContext &context) : Parser(context.GetParserOptions()) {
}

Parser::Parser(ClientContext &context, IdentifierCaseMode identifier_case_mode) : Parser(context) {
	options.identifier_case_mode = identifier_case_mode;
}

const CompiledGrammar &CompiledGrammar::Get(const ClientContext &context) {
	return context.IsConnected() ? Passthrough() : Base();
}

} // namespace duckdb
