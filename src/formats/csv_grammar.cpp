#include "duckdb/formats/csv_grammar.hpp"

namespace duckdb {

CsvGrammar CsvGrammar::For(const CSVStateMachine &state_machine) {
	CsvGrammar grammar;
	auto &dialect = state_machine.dialect_options.state_machine_options;
	const auto delimiter = dialect.delimiter.GetValue();
	grammar.delimiter_size = MinValue<idx_t>(MaxValue<idx_t>(delimiter.size(), 1), 4);
	for (idx_t i = 0; i < grammar.delimiter_size && i < delimiter.size(); i++) {
		grammar.delimiter[i] = delimiter[i];
	}
	grammar.quote = dialect.quote.GetValue();
	grammar.escape = dialect.escape.GetValue();
	grammar.comment = dialect.comment.GetValue();
	grammar.new_line = dialect.new_line.GetValue();
	grammar.strict_mode = dialect.strict_mode.GetValue();
	grammar.carry_on_newline = grammar.new_line == NewLineIdentifier::CARRY_ON;
	grammar.empty_space = grammar.delimiter[0] != ' ';
	grammar.trim_quoted_spaces =
	    grammar.empty_space && grammar.quote != ' ' && grammar.escape != ' ' && grammar.comment != ' ';
	grammar.quoted_escape = grammar.escape != '\0' && grammar.escape != grammar.quote;
	grammar.unquoted_escape = grammar.quoted_escape && !grammar.strict_mode;
	for (idx_t i = 0; i < grammar.delimiter_size; i++) {
		for (idx_t j = i + 1; j < grammar.delimiter_size; j++) {
			grammar.distinct_delimiter = grammar.distinct_delimiter && grammar.delimiter[i] != grammar.delimiter[j];
		}
	}
	return grammar;
}

string CsvStripSeparator(const char *value, idx_t size, char separator) {
	string result;
	result.reserve(size);
	for (idx_t i = 0; i < size; i++) {
		if (value[i] != separator) {
			result.push_back(value[i]);
		}
	}
	return result;
}

} // namespace duckdb
