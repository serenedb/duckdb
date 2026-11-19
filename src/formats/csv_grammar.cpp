#include "duckdb/formats/csv_grammar.hpp"

#include "duckdb/common/bit_utils.hpp"
#include "duckdb/formats/csv_simd.hpp"

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

namespace {

constexpr idx_t SYNC_WINDOW = 1 << 14;

idx_t FirstNewline(const CsvGrammar &grammar, const char *bytes, idx_t base, idx_t begin, idx_t end, bool eof) {
	const auto at = [&](idx_t position) {
		return bytes[position - base];
	};
	for (idx_t block = begin; block < end; block += CSV_BLOCK_BYTES) {
		const idx_t size = MinValue<idx_t>(CSV_BLOCK_BYTES, end - block);
		char padded[CSV_BLOCK_BYTES];
		const char *data = bytes + (block - base);
		if (size < CSV_BLOCK_BYTES) {
			memset(padded, 0, sizeof(padded));
			memcpy(padded, data, size);
			data = padded;
		}
		const CsvVector vector(data);
		uint64_t candidates = vector.Equal('\n') | vector.Equal('\r');
		if (size < CSV_BLOCK_BYTES) {
			candidates &= (uint64_t(1) << size) - 1;
		}
		while (candidates) {
			const idx_t position = block + CountZeros<uint64_t>::Trailing(candidates);
			candidates &= candidates - 1;
			if (at(position) == '\r') {
				if (position + 1 >= end) {
					if (!eof) {
						return DConstants::INVALID_INDEX;
					}
				} else if (at(position + 1) == '\n') {
					continue;
				}
			}
			if (grammar.quoted_escape) {
				if (position > base && at(position - 1) == grammar.escape) {
					continue;
				}
				if (position > base + 1 && at(position - 1) == '\r' && at(position - 2) == grammar.escape) {
					continue;
				}
			}
			return position + 1;
		}
	}
	return DConstants::INVALID_INDEX;
}

bool OpensRecord(const CsvGrammar &grammar, const char *bytes, idx_t base, idx_t candidate, idx_t limit) {
	const auto at = [&](idx_t position) {
		return bytes[position - base];
	};
	const auto found = static_cast<const char *>(memchr(bytes + (candidate - base), grammar.quote, limit - candidate));
	if (!found) {
		return false;
	}
	const idx_t quote = static_cast<idx_t>(found - bytes) + base;
	if (quote + 1 >= limit) {
		return false;
	}
	const char before = quote == candidate ? '\n' : at(quote - 1);
	const char after = at(quote + 1);
	const char last_delimiter = grammar.delimiter[grammar.delimiter_size - 1];
	const bool field_start = before == last_delimiter || before == '\n' || before == '\r';
	const bool data_follows = after != grammar.delimiter[0] && after != '\n' && after != '\r' &&
	                          after != grammar.quote && after != grammar.escape;
	return field_start && data_follows;
}

} // namespace

idx_t CsvSync(const CsvGrammar &grammar, const char *bytes, idx_t base, idx_t begin, idx_t end, bool eof) {
	const idx_t first = FirstNewline(grammar, bytes, base, begin, end, eof);
	if (first == DConstants::INVALID_INDEX || !grammar.quote) {
		return first;
	}
	const idx_t window = first + SYNC_WINDOW;
	if (window > end && !eof) {
		return DConstants::INVALID_INDEX;
	}
	const idx_t limit = MinValue(window, end);
	for (idx_t candidate = first; candidate < limit;) {
		if (OpensRecord(grammar, bytes, base, candidate, limit)) {
			return candidate;
		}
		const idx_t next = FirstNewline(grammar, bytes, base, candidate, limit, true);
		if (next == DConstants::INVALID_INDEX || next >= limit) {
			break;
		}
		candidate = next;
	}
	return first;
}

} // namespace duckdb
