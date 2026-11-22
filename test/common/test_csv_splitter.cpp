#include "catch.hpp"
#include "duckdb/common/bit_utils.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_state_machine.hpp"
#include "duckdb/formats/csv_simd.hpp"
#include "duckdb/formats/row_lexer.hpp"

#include <fstream>
#include <random>

using namespace duckdb;

namespace {

struct ReferenceTable {
	static constexpr idx_t STATES = static_cast<idx_t>(CSVState::MAYBE_QUOTED) + 1;
	static constexpr idx_t BYTES = 256;

	CSVState *operator[](idx_t byte) {
		return state_machine[byte];
	}
	const CSVState *operator[](idx_t byte) const {
		return state_machine[byte];
	}

	CSVState state_machine[BYTES][STATES];
};

struct ReferenceGrammar : CsvGrammar {
	ReferenceTable table;
	idx_t extra_delimiter_bytes = 0;
};

void BuildReferenceTable(const CSVStateMachineOptions &options, ReferenceTable &table) {
	const bool strict = options.strict_mode.GetValue();
	const auto fill = [&](CSVState state, CSVState target) {
		for (idx_t byte = 0; byte < ReferenceTable::BYTES; byte++) {
			table[byte][static_cast<uint8_t>(state)] = target;
		}
	};
	for (idx_t i = 0; i < ReferenceTable::STATES; i++) {
		const auto state = static_cast<CSVState>(i);
		switch (state) {
		case CSVState::MAYBE_QUOTED:
		case CSVState::QUOTED:
		case CSVState::QUOTED_NEW_LINE:
		case CSVState::ESCAPE:
			fill(state, CSVState::QUOTED);
			break;
		case CSVState::UNQUOTED:
			fill(state, strict ? CSVState::INVALID : CSVState::UNQUOTED);
			break;
		case CSVState::COMMENT:
			fill(state, CSVState::COMMENT);
			break;
		case CSVState::CARRIAGE_RETURN:
			fill(state, strict ? CSVState::INVALID : CSVState::STANDARD);
			break;
		default:
			fill(state, CSVState::STANDARD);
			break;
		}
	}
	const auto at = [&](uint8_t byte, CSVState state) -> CSVState & {
		return table[byte][static_cast<uint8_t>(state)];
	};
	const auto delimiter_value = options.delimiter.GetValue();
	const uint8_t delimiter_first_byte = delimiter_value.empty() ? 0 : static_cast<uint8_t>(delimiter_value[0]);
	const auto quote = static_cast<uint8_t>(options.quote.GetValue());
	const auto escape = static_cast<uint8_t>(options.escape.GetValue());
	const auto comment = static_cast<uint8_t>(options.comment.GetValue());
	const auto new_line_id = options.new_line.GetValue();
	const bool multi_byte_delimiter = delimiter_value.size() > 1;
	const bool enable_unquoted_escape = !strict && quote != escape && escape != 0;
	const auto first_delimiter = multi_byte_delimiter ? CSVState::DELIMITER_FIRST_BYTE : CSVState::DELIMITER;
	const uint8_t lf = '\n';
	const uint8_t cr = '\r';
	const uint8_t space = ' ';

	for (const auto state : {CSVState::STANDARD, CSVState::INVALID, CSVState::STANDARD_NEWLINE}) {
		at(delimiter_first_byte, state) = first_delimiter;
		if (new_line_id == NewLineIdentifier::CARRY_ON) {
			at(cr, state) = CSVState::CARRIAGE_RETURN;
			if (state == CSVState::STANDARD_NEWLINE) {
				at(lf, state) = CSVState::STANDARD;
			} else if (!strict) {
				at(lf, state) = CSVState::RECORD_SEPARATOR;
			} else {
				at(lf, state) = CSVState::INVALID;
			}
		} else if (new_line_id == NewLineIdentifier::SINGLE_N) {
			at(lf, state) = CSVState::RECORD_SEPARATOR;
			at(cr, state) = strict ? CSVState::INVALID : CSVState::RECORD_SEPARATOR;
		} else if (new_line_id == NewLineIdentifier::SINGLE_R) {
			at(cr, state) = CSVState::RECORD_SEPARATOR;
			at(lf, state) = strict ? CSVState::INVALID : CSVState::RECORD_SEPARATOR;
		} else {
			at(cr, state) = CSVState::RECORD_SEPARATOR;
			at(lf, state) = CSVState::RECORD_SEPARATOR;
		}
		if (comment != 0) {
			at(comment, state) = CSVState::COMMENT;
		}
		if (enable_unquoted_escape) {
			at(escape, state) = CSVState::UNQUOTED_ESCAPE;
		}
	}

	if (quote != 0) {
		at(quote, CSVState::DELIMITER) = CSVState::QUOTED;
	}
	if (delimiter_first_byte != space) {
		at(space, CSVState::DELIMITER) = CSVState::EMPTY_SPACE;
	}
	for (const auto state : {CSVState::DELIMITER, CSVState::DELIMITER_FIRST_BYTE, CSVState::DELIMITER_SECOND_BYTE,
	                         CSVState::DELIMITER_THIRD_BYTE}) {
		at(delimiter_first_byte, state) = first_delimiter;
		at(lf, state) = CSVState::RECORD_SEPARATOR;
		at(cr, state) =
		    new_line_id == NewLineIdentifier::CARRY_ON ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR;
		if (comment != 0) {
			at(comment, CSVState::DELIMITER) = CSVState::COMMENT;
		}
	}
	if (delimiter_value.size() == 2) {
		at(delimiter_value[1], CSVState::DELIMITER_FIRST_BYTE) = CSVState::DELIMITER;
	} else if (delimiter_value.size() == 3) {
		if (delimiter_first_byte == delimiter_value[1]) {
			at(delimiter_value[1], CSVState::DELIMITER_SECOND_BYTE) = CSVState::DELIMITER_SECOND_BYTE;
		}
		at(delimiter_value[1], CSVState::DELIMITER_FIRST_BYTE) = CSVState::DELIMITER_SECOND_BYTE;
		at(delimiter_value[2], CSVState::DELIMITER_SECOND_BYTE) = CSVState::DELIMITER;
	} else if (delimiter_value.size() == 4) {
		if (delimiter_first_byte == delimiter_value[2]) {
			at(delimiter_value[1], CSVState::DELIMITER_THIRD_BYTE) = CSVState::DELIMITER_SECOND_BYTE;
		}
		if (delimiter_first_byte == delimiter_value[1] && delimiter_value[1] == delimiter_value[2]) {
			at(delimiter_value[1], CSVState::DELIMITER_THIRD_BYTE) = CSVState::DELIMITER_THIRD_BYTE;
		}
		at(delimiter_value[1], CSVState::DELIMITER_FIRST_BYTE) = CSVState::DELIMITER_SECOND_BYTE;
		at(delimiter_value[2], CSVState::DELIMITER_SECOND_BYTE) = CSVState::DELIMITER_THIRD_BYTE;
		at(delimiter_value[3], CSVState::DELIMITER_THIRD_BYTE) = CSVState::DELIMITER;
	}
	if (enable_unquoted_escape) {
		at(escape, CSVState::DELIMITER) = CSVState::UNQUOTED_ESCAPE;
	}

	at(delimiter_first_byte, CSVState::RECORD_SEPARATOR) = first_delimiter;
	at(lf, CSVState::RECORD_SEPARATOR) = CSVState::RECORD_SEPARATOR;
	at(cr, CSVState::RECORD_SEPARATOR) =
	    new_line_id == NewLineIdentifier::CARRY_ON ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR;
	if (quote != 0) {
		at(quote, CSVState::RECORD_SEPARATOR) = CSVState::QUOTED;
	}
	if (delimiter_first_byte != space) {
		at(space, CSVState::RECORD_SEPARATOR) = CSVState::EMPTY_SPACE;
	}
	if (comment != 0) {
		at(comment, CSVState::RECORD_SEPARATOR) = CSVState::COMMENT;
	}
	if (enable_unquoted_escape) {
		at(escape, CSVState::RECORD_SEPARATOR) = CSVState::UNQUOTED_ESCAPE;
	}
	if (strict) {
		if (new_line_id == NewLineIdentifier::CARRY_ON || new_line_id == NewLineIdentifier::SINGLE_R) {
			at(lf, CSVState::RECORD_SEPARATOR) = CSVState::INVALID;
		} else if (new_line_id == NewLineIdentifier::SINGLE_N) {
			at(cr, CSVState::RECORD_SEPARATOR) = CSVState::INVALID;
		}
	}

	at(lf, CSVState::CARRIAGE_RETURN) = CSVState::RECORD_SEPARATOR;
	at(cr, CSVState::CARRIAGE_RETURN) = strict ? CSVState::INVALID : CSVState::CARRIAGE_RETURN;
	if (quote != 0) {
		at(quote, CSVState::CARRIAGE_RETURN) = CSVState::QUOTED;
	}
	if (delimiter_first_byte != space) {
		at(space, CSVState::CARRIAGE_RETURN) = CSVState::EMPTY_SPACE;
	}
	if (comment != 0) {
		at(comment, CSVState::CARRIAGE_RETURN) = CSVState::COMMENT;
	}
	if (enable_unquoted_escape) {
		at(escape, CSVState::CARRIAGE_RETURN) = CSVState::UNQUOTED_ESCAPE;
	}

	at(quote, CSVState::QUOTED) = CSVState::UNQUOTED;
	at(lf, CSVState::QUOTED) = CSVState::QUOTED_NEW_LINE;
	at(cr, CSVState::QUOTED) = CSVState::QUOTED_NEW_LINE;
	if (quote != escape && escape != 0) {
		at(escape, CSVState::QUOTED) = CSVState::ESCAPE;
	}

	at(lf, CSVState::UNQUOTED) = CSVState::RECORD_SEPARATOR;
	at(cr, CSVState::UNQUOTED) =
	    new_line_id == NewLineIdentifier::CARRY_ON ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR;
	at(delimiter_first_byte, CSVState::UNQUOTED) = first_delimiter;
	if (quote == escape) {
		at(quote, CSVState::UNQUOTED) = CSVState::QUOTED;
	}
	if (strict) {
		if (new_line_id == NewLineIdentifier::CARRY_ON || new_line_id == NewLineIdentifier::SINGLE_R) {
			at(lf, CSVState::UNQUOTED) = CSVState::INVALID;
		} else if (new_line_id == NewLineIdentifier::SINGLE_N) {
			at(cr, CSVState::UNQUOTED) = CSVState::INVALID;
		}
	} else {
		at(quote, CSVState::UNQUOTED) = escape == 0 ? CSVState::MAYBE_QUOTED : CSVState::QUOTED;
	}
	if (comment != 0) {
		at(comment, CSVState::UNQUOTED) = CSVState::COMMENT;
	}
	if (delimiter_first_byte != space && quote != space && escape != space && comment != space) {
		at(space, CSVState::UNQUOTED) = CSVState::UNQUOTED;
	}

	at(delimiter_first_byte, CSVState::NOT_SET) = first_delimiter;
	at(lf, CSVState::NOT_SET) = CSVState::RECORD_SEPARATOR;
	at(cr, CSVState::NOT_SET) =
	    new_line_id == NewLineIdentifier::CARRY_ON ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR;
	if (quote != 0) {
		at(quote, CSVState::NOT_SET) = CSVState::QUOTED;
	}
	if (delimiter_first_byte != space) {
		at(space, CSVState::NOT_SET) = CSVState::EMPTY_SPACE;
	}
	if (comment != 0) {
		at(comment, CSVState::NOT_SET) = CSVState::COMMENT;
	}
	if (enable_unquoted_escape) {
		at(escape, CSVState::NOT_SET) = CSVState::UNQUOTED_ESCAPE;
	}

	at(quote, CSVState::QUOTED_NEW_LINE) = CSVState::UNQUOTED;
	if (quote != escape && escape != 0) {
		at(escape, CSVState::QUOTED_NEW_LINE) = CSVState::ESCAPE;
	}

	at(delimiter_first_byte, CSVState::EMPTY_SPACE) = first_delimiter;
	at(lf, CSVState::EMPTY_SPACE) = CSVState::RECORD_SEPARATOR;
	at(cr, CSVState::EMPTY_SPACE) =
	    new_line_id == NewLineIdentifier::CARRY_ON ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR;
	if (quote != 0) {
		at(quote, CSVState::EMPTY_SPACE) = CSVState::QUOTED;
	}
	if (comment != 0) {
		at(comment, CSVState::EMPTY_SPACE) = CSVState::COMMENT;
	}
	if (enable_unquoted_escape) {
		at(escape, CSVState::EMPTY_SPACE) = CSVState::UNQUOTED_ESCAPE;
	}

	at(lf, CSVState::COMMENT) = CSVState::RECORD_SEPARATOR;
	at(cr, CSVState::COMMENT) =
	    new_line_id == NewLineIdentifier::CARRY_ON ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR;

	if (enable_unquoted_escape && new_line_id == NewLineIdentifier::CARRY_ON) {
		at(cr, CSVState::UNQUOTED_ESCAPE) = CSVState::ESCAPED_RETURN;
	}

	if (enable_unquoted_escape) {
		at(delimiter_first_byte, CSVState::ESCAPED_RETURN) = CSVState::DELIMITER;
		at(cr, CSVState::ESCAPED_RETURN) =
		    new_line_id == NewLineIdentifier::CARRY_ON ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR;
		if (comment != 0) {
			at(comment, CSVState::ESCAPED_RETURN) = CSVState::COMMENT;
		}
		at(escape, CSVState::ESCAPED_RETURN) = CSVState::UNQUOTED_ESCAPE;
	}

	at(quote, CSVState::MAYBE_QUOTED) = CSVState::MAYBE_QUOTED;
	at(lf, CSVState::MAYBE_QUOTED) = CSVState::RECORD_SEPARATOR;
	at(cr, CSVState::MAYBE_QUOTED) =
	    new_line_id == NewLineIdentifier::CARRY_ON ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR;
	at(delimiter_first_byte, CSVState::MAYBE_QUOTED) = first_delimiter;
}

ReferenceGrammar MakeReferenceGrammar(const CSVStateMachine &machine) {
	ReferenceGrammar grammar;
	static_cast<CsvGrammar &>(grammar) = CsvGrammar::For(machine);
	const auto &options = machine.dialect_options.state_machine_options;
	BuildReferenceTable(options, grammar.table);
	const auto delimiter = options.delimiter.GetValue();
	grammar.extra_delimiter_bytes = delimiter.empty() ? 0 : delimiter.size() - 1;
	return grammar;
}

enum CsvRowFlags : uint8_t {
	CSV_ROW_EMPTY_LINE = 1,
	CSV_ROW_QUOTED_NEWLINE = 2,
	CSV_ROW_EVENTS = 4,
	CSV_ROW_UNFINISHED = 8,
};

struct ReferenceCsvRow {
	idx_t start;
	idx_t begin;
	idx_t end;
	idx_t last_position;
	idx_t lines;
	uint32_t field_count;
	uint8_t flags;
};

struct CsvExtraField {
	idx_t row;
	idx_t column;
	CsvField field;
};

enum class CsvRowEventKind : uint8_t {
	UNTERMINATED_QUOTES,
	INVALID_STATE,
};

struct CsvRowEvent {
	idx_t row;
	idx_t column;
	idx_t position;
	CsvRowEventKind kind;
};

class ReferenceSplitter {
public:
	ReferenceSplitter(const ReferenceGrammar &grammar_p, idx_t columns_p, idx_t capacity_p)
	    : grammar(grammar_p), columns(columns_p), capacity(capacity_p), fields(columns_p * capacity_p),
	      rows(capacity_p) {
	}

	void Reset(idx_t position, idx_t lines, const CsvRangeStart &start = CsvRangeStart()) {
		pos = position;
		states[0] = start.states[0];
		states[1] = start.states[1];
		field_count = 0;
		last_position = start.last_position == DConstants::INVALID_INDEX ? position : start.last_position;
		quoted = false;
		ever_quoted = false;
		ever_escaped = false;
		used_unstrictness = false;
		escaped = false;
		comment = false;
		row_flags = 0;
		line_end = start.line_end == DConstants::INVALID_INDEX ? position : start.line_end;
		line_begin = line_end;
		lines_read = lines;
		finished = false;
		SaveBoundary();
	}

	idx_t Split(const char *bytes_p, idx_t base_p, idx_t end_p, idx_t max_rows, bool last,
	            idx_t stop_at = NumericLimits<idx_t>::Maximum()) {
		bytes = bytes_p;
		base = base_p;
		end = end_p;
		row_count = 0;
		events.clear();
		extras.clear();
		if (finished) {
			return row_count;
		}
		const bool has_escape = grammar.escape != '\0';
		while (pos < end && row_count < max_rows) {
			if (pos >= stop_at && pos == boundary.pos) {
				finished = true;
				return row_count;
			}
			scan_pos = pos;
			states[0] = states[1];
			states[1] = grammar.table[static_cast<uint8_t>(At(pos))][static_cast<uint8_t>(states[1])];
			switch (states[1]) {
			case CSVState::INVALID:
				Event(quoted ? CsvRowEventKind::UNTERMINATED_QUOTES : CsvRowEventKind::INVALID_STATE, field_count,
				      last_position);
				pos++;
				break;
			case CSVState::RECORD_SEPARATOR:
				if (states[0] == CSVState::RECORD_SEPARATOR || states[0] == CSVState::NOT_SET) {
					EmptyLine(pos);
					lines_read++;
				} else if (states[0] != CSVState::CARRIAGE_RETURN) {
					if (comment) {
						UnsetComment(pos);
					} else {
						AddRow(pos);
					}
					lines_read++;
				}
				pos++;
				SaveBoundary();
				break;
			case CSVState::CARRIAGE_RETURN:
				if (states[0] == CSVState::RECORD_SEPARATOR || states[0] == CSVState::NOT_SET) {
					EmptyLine(pos);
				} else if (states[0] != CSVState::CARRIAGE_RETURN) {
					if (comment) {
						UnsetComment(pos);
					} else {
						AddRow(pos);
					}
				}
				if (fixed) {
					last_position = pos + 1 + FollowedByLineFeed(pos);
				}
				pos++;
				lines_read++;
				SaveBoundary();
				break;
			case CSVState::DELIMITER:
				AddValue(pos);
				pos++;
				break;
			case CSVState::QUOTED:
				if ((states[0] == CSVState::UNQUOTED || states[0] == CSVState::MAYBE_QUOTED) && has_escape) {
					escaped = true;
					ever_escaped = true;
				}
				if ((states[0] == CSVState::ESCAPE || states[0] == CSVState::ESCAPED_RETURN ||
				     states[0] == CSVState::UNQUOTED_ESCAPE) &&
				    (At(pos) == grammar.quote || !grammar.strict_mode)) {
					ever_escaped = true;
					used_unstrictness = used_unstrictness || states[0] == CSVState::UNQUOTED_ESCAPE;
				}
				if (!quoted) {
					quoted_position = pos;
				}
				quoted = true;
				ever_quoted = true;
				pos++;
				break;
			case CSVState::UNQUOTED:
				if (states[0] == CSVState::MAYBE_QUOTED || (states[0] == CSVState::UNQUOTED && has_escape)) {
					escaped = true;
				}
				ever_escaped = ever_escaped || states[0] == CSVState::MAYBE_QUOTED;
				quoted = true;
				pos++;
				break;
			case CSVState::ESCAPE:
			case CSVState::ESCAPED_RETURN:
				escaped = true;
				pos++;
				break;
			case CSVState::UNQUOTED_ESCAPE:
				escaped = true;
				used_unstrictness = true;
				pos++;
				break;
			case CSVState::QUOTED_NEW_LINE:
				row_flags |= CSV_ROW_QUOTED_NEWLINE;
				pos++;
				break;
			case CSVState::COMMENT:
				if (!comment) {
					position_before_comment = pos;
					comment = true;
				}
				pos++;
				break;
			default:
				pos++;
				break;
			}
		}
		if (pos >= stop_at && pos == boundary.pos) {
			finished = true;
			return row_count;
		}
		if (pos < end || row_count == max_rows) {
			return row_count;
		}
		if (!last) {
			RestoreBoundary();
			return row_count;
		}
		Finish();
		return row_count;
	}

	struct Boundary {
		idx_t pos = 0;
		CSVState states[2] = {CSVState::NOT_SET, CSVState::NOT_SET};
		idx_t last_position = 0;
		idx_t line_begin = 0;
		idx_t line_end = 0;
		idx_t lines_read = 0;
		bool ever_quoted = false;
	};

	char At(idx_t position) const {
		return bytes[position - base];
	}

	void SaveBoundary() {
		boundary.pos = pos;
		boundary.states[0] = states[0];
		boundary.states[1] = states[1];
		boundary.last_position = last_position;
		boundary.line_begin = line_begin;
		boundary.line_end = line_end;
		boundary.lines_read = lines_read;
		boundary.ever_quoted = ever_quoted;
	}

	void RestoreBoundary() {
		pos = boundary.pos;
		states[0] = boundary.states[0];
		states[1] = boundary.states[1];
		last_position = boundary.last_position;
		line_begin = boundary.line_begin;
		line_end = boundary.line_end;
		lines_read = boundary.lines_read;
		ever_quoted = boundary.ever_quoted;
		field_count = 0;
		quoted = false;
		escaped = false;
		comment = false;
		row_flags = 0;
		while (!events.empty() && events.back().row >= row_count) {
			events.pop_back();
		}
		while (!extras.empty() && extras.back().row >= row_count) {
			extras.pop_back();
		}
	}

	void Event(CsvRowEventKind kind, idx_t column, idx_t position) {
		events.push_back({row_count, column, position, kind});
		row_flags |= CSV_ROW_EVENTS;
	}

	void AddField(idx_t offset, idx_t size, bool empty) {
		CsvField field {static_cast<uint32_t>(last_position - base), static_cast<uint32_t>(offset - base),
		                static_cast<uint32_t>(empty ? 0 : size), static_cast<uint8_t>(0)};
		if (quoted) {
			field.flags |= CSV_FIELD_QUOTED;
		}
		if (escaped) {
			field.flags |= CSV_FIELD_ESCAPED;
		}
		if (field_count < columns) {
			fields[field_count * capacity + row_count] = field;
		} else {
			extras.push_back({row_count, field_count, field});
		}
		field_count++;
		escaped = false;
	}

	void AddQuotedValue(idx_t position) {
		if (!ever_quoted) {
			Event(CsvRowEventKind::UNTERMINATED_QUOTES, field_count, last_position);
		}
		if (position <= quoted_position + 1) {
			AddField(quoted_position + 1, 0, position < last_position + 2);
			quoted = false;
			return;
		}
		idx_t length = position - quoted_position - 1;
		while (length > 0 && grammar.trim_quoted_spaces && At(quoted_position + 1 + length - 1) == ' ') {
			length--;
		}
		if (length == 0) {
			AddField(quoted_position + 1, 0, true);
			quoted = false;
			return;
		}
		length--;
		AddField(quoted_position + 1, length, position < last_position + 2);
		quoted = false;
	}

	void AddValue(idx_t position) {
		if (last_position > position) {
			return;
		}
		if (quoted) {
			AddQuotedValue(position - grammar.extra_delimiter_bytes);
		} else if (escaped && !fixed) {
			AddField(last_position, position - last_position, false);
		} else {
			idx_t size = 0;
			if (position >= last_position + grammar.extra_delimiter_bytes) {
				size = position - last_position - grammar.extra_delimiter_bytes;
			}
			AddField(last_position, size, false);
		}
		last_position = position + 1;
	}

	void AddRow(idx_t position) {
		if (last_position <= position) {
			if (quoted) {
				AddQuotedValue(position);
			} else {
				const idx_t size = position - last_position;
				AddField(last_position, size, escaped && size == 0);
			}
			if (grammar.carry_on_newline && states[1] != CSVState::RECORD_SEPARATOR) {
				last_position = position + 2;
			} else {
				last_position = position + 1;
			}
		}
		line_begin = line_end;
		line_end = scan_pos;
		rows[row_count] = {boundary.pos,  line_begin, line_end,
		                   last_position, lines_read, static_cast<uint32_t>(field_count),
		                   row_flags};
		row_count++;
		field_count = 0;
		row_flags = 0;
	}

	void EmptyLine(idx_t position) {
		last_position = position + 1;
		if (states[1] == CSVState::CARRIAGE_RETURN && grammar.carry_on_newline) {
			last_position++;
		}
		if (columns != 1) {
			return;
		}
		rows[row_count] = {boundary.pos, line_end, line_end, last_position, lines_read, 0, CSV_ROW_EMPTY_LINE};
		row_count++;
	}

	void UnsetComment(idx_t position) {
		if (last_position < position_before_comment) {
			bool all_empty = true;
			for (idx_t i = last_position; i < position_before_comment; i++) {
				if (At(i) != ' ') {
					all_empty = false;
					break;
				}
			}
			if (!all_empty) {
				AddRow(position_before_comment);
			}
		} else if (field_count != 0) {
			AddRow(position_before_comment);
		}
		comment = false;
		last_position = grammar.carry_on_newline ? position + 2 : position + 1;
		if (fixed && grammar.carry_on_newline && !(At(position) == '\r' && FollowedByLineFeed(position))) {
			last_position = position + 1;
		}
		line_begin = line_end;
		line_end = scan_pos;
		field_count = 0;
	}

	bool FollowedByLineFeed(idx_t position) const {
		return position + 1 < total && bytes[position + 1 - base] == '\n';
	}

	void Finish() {
		finished = true;
		scan_pos = end;
		if (states[1] == CSVState::CARRIAGE_RETURN || states[1] == CSVState::RECORD_SEPARATOR ||
		    states[1] == CSVState::NOT_SET) {
			field_count = 0;
			return;
		}
		if (states[1] == CSVState::DELIMITER) {
			AddValue(end);
			if (comment) {
				UnsetComment(end);
			} else {
				AddRow(end);
			}
			lines_read++;
			return;
		}
		if ((states[1] == CSVState::QUOTED || states[1] == CSVState::QUOTED_NEW_LINE) && grammar.strict_mode) {
			line_begin = line_end;
			line_end = end;
			Event(quoted ? CsvRowEventKind::UNTERMINATED_QUOTES : CsvRowEventKind::INVALID_STATE, field_count,
			      last_position);
			rows[row_count] = {boundary.pos,
			                   line_begin,
			                   line_end,
			                   last_position,
			                   lines_read,
			                   static_cast<uint32_t>(field_count),
			                   static_cast<uint8_t>(row_flags | CSV_ROW_UNFINISHED)};
			row_count++;
			field_count = 0;
			row_flags = 0;
			return;
		}
		if (states[1] == CSVState::QUOTED || states[1] == CSVState::QUOTED_NEW_LINE) {
			used_unstrictness = true;
		}
		if (comment) {
			UnsetComment(end);
		} else {
			if (quoted && grammar.strict_mode &&
			    (states[0] == CSVState::DELIMITER_FIRST_BYTE || states[0] == CSVState::DELIMITER_SECOND_BYTE ||
			     states[0] == CSVState::DELIMITER_THIRD_BYTE)) {
				Event(CsvRowEventKind::UNTERMINATED_QUOTES, field_count, last_position);
			}
			AddRow(end);
		}
		lines_read++;
	}

	const ReferenceGrammar &grammar;
	const idx_t columns;
	const idx_t capacity;
	bool fixed = false;
	idx_t total = 0;
	vector<CsvField> fields;
	vector<CsvExtraField> extras;
	vector<ReferenceCsvRow> rows;
	vector<CsvRowEvent> events;
	Boundary boundary;
	const char *bytes = nullptr;
	idx_t base = 0;
	idx_t end = 0;
	idx_t pos = 0;
	idx_t scan_pos = 0;
	idx_t row_count = 0;
	idx_t lines_read = 0;
	CSVState states[2] = {CSVState::NOT_SET, CSVState::NOT_SET};
	idx_t field_count = 0;
	idx_t last_position = 0;
	idx_t quoted_position = 0;
	bool quoted = false;
	bool ever_quoted = false;
	bool ever_escaped = false;
	bool used_unstrictness = false;
	bool escaped = false;
	bool comment = false;
	idx_t position_before_comment = 0;
	uint8_t row_flags = 0;
	idx_t line_begin = 0;
	idx_t line_end = 0;
	bool finished = false;
};

void ReferenceTransfer(const ReferenceGrammar &grammar, const char *bytes, idx_t base, idx_t begin, idx_t end,
                       CsvRangeTransfer &transfer, bool fixed = false) {
	struct Run {
		CsvRangeStart start;
		uint32_t origins;
	};
	constexpr idx_t STATES = ReferenceTable::STATES;
	constexpr idx_t NONE = DConstants::INVALID_INDEX;
	const auto &table = grammar.table;
	Run runs[STATES];
	CsvRangeStart settled[STATES];
	idx_t run_count = STATES;
	uint32_t open = (1u << STATES) - 1;
	for (idx_t state = 0; state < STATES; state++) {
		runs[state].start.states[0] = static_cast<CSVState>(state);
		runs[state].start.states[1] = static_cast<CSVState>(state);
		runs[state].origins = 1u << state;
		transfer[state] = CsvRangeEntry();
	}
	auto settle = [&](const Run &run) {
		for (uint32_t origins = run.origins; origins; origins &= origins - 1) {
			auto &origin = settled[CountZeros<uint32_t>::Trailing(origins)];
			if (run.start.line_end != NONE) {
				origin.line_end = run.start.line_end;
			}
			if (run.start.last_position != NONE) {
				origin.last_position = run.start.last_position;
			}
		}
	};
	auto resolve = [&](const Run &run, idx_t origin) {
		auto result = run.start;
		result.Inherit(settled[origin]);
		return result;
	};
	for (idx_t pos = begin; pos < end; pos++) {
		const auto byte = static_cast<uint8_t>(bytes[pos - base]);
		for (idx_t i = 0; i < run_count; i++) {
			auto &run = runs[i];
			const auto previous = run.start.states[1];
			const auto next = table.state_machine[byte][static_cast<uint8_t>(previous)];
			run.start.states[0] = previous;
			run.start.states[1] = next;
			if (next != CSVState::RECORD_SEPARATOR && next != CSVState::CARRIAGE_RETURN) {
				continue;
			}
			const bool empty = previous == CSVState::RECORD_SEPARATOR || previous == CSVState::NOT_SET;
			const bool comment = previous == CSVState::COMMENT;
			if (previous != CSVState::CARRIAGE_RETURN) {
				if (!empty) {
					run.start.line_end = pos;
				}
				const bool skip_line_feed = next == CSVState::CARRIAGE_RETURN || (comment && grammar.carry_on_newline);
				run.start.last_position = pos + 1 + skip_line_feed;
			}
			if (fixed) {
				run.start.last_position = pos + 1;
			}
			if (!(run.origins & open)) {
				continue;
			}
			for (uint32_t origins = run.origins & open; origins; origins &= origins - 1) {
				const auto origin = CountZeros<uint32_t>::Trailing(origins);
				auto &entry = transfer[origin];
				entry.first_boundary = pos + 1;
				entry.boundary = resolve(run, origin);
			}
			open &= ~run.origins;
		}
		if (run_count > 1) {
			idx_t kept = 0;
			for (idx_t i = 0; i < run_count; i++) {
				bool merged = false;
				for (idx_t j = 0; j < kept; j++) {
					auto &target = runs[j];
					auto &source = runs[i];
					if (target.start.states[0] != source.start.states[0] ||
					    target.start.states[1] != source.start.states[1]) {
						continue;
					}
					if (target.start.line_end != source.start.line_end ||
					    target.start.last_position != source.start.last_position) {
						settle(target);
						settle(source);
						target.start.line_end = NONE;
						target.start.last_position = NONE;
					}
					target.origins |= source.origins;
					merged = true;
					break;
				}
				if (!merged) {
					runs[kept++] = runs[i];
				}
			}
			run_count = kept;
		}
	}
	for (idx_t i = 0; i < run_count; i++) {
		for (uint32_t origins = runs[i].origins; origins; origins &= origins - 1) {
			const auto origin = CountZeros<uint32_t>::Trailing(origins);
			transfer[origin].end = resolve(runs[i], origin);
		}
	}
}

class ReferenceSkip {
public:
	ReferenceSkip(const ReferenceGrammar &grammar_p, idx_t rows_p) : grammar(grammar_p), rows(rows_p) {
	}

	bool Feed(const char *data, idx_t base, idx_t end) {
		auto &table = grammar.table;
		while (pos < end) {
			states[0] = states[1];
			states[1] = table.state_machine[static_cast<uint8_t>(data[pos - base])][static_cast<uint8_t>(states[1])];
			bool done = false;
			switch (states[1]) {
			case CSVState::RECORD_SEPARATOR:
				if (states[0] == CSVState::RECORD_SEPARATOR || states[0] == CSVState::NOT_SET) {
					done = AddRow();
					lines++;
				} else if (states[0] != CSVState::CARRIAGE_RETURN) {
					done = EndLine(true);
					lines++;
				}
				break;
			case CSVState::CARRIAGE_RETURN:
				if (states[0] == CSVState::RECORD_SEPARATOR || states[0] == CSVState::NOT_SET) {
					done = AddRow();
				} else if (states[0] != CSVState::CARRIAGE_RETURN) {
					done = EndLine(true);
				}
				lines++;
				break;
			case CSVState::COMMENT:
				if (!comment && states[0] != CSVState::STANDARD) {
					starts_as_comment = true;
				}
				comment = true;
				break;
			default:
				break;
			}
			pos++;
			if (done) {
				return true;
			}
		}
		return false;
	}

	idx_t Start(const string &input) const {
		if (grammar.carry_on_newline && states[1] == CSVState::CARRIAGE_RETURN && pos < input.size() &&
		    input[pos] == '\n') {
			return pos + 1;
		}
		return pos;
	}

	bool AddRow() {
		skipped++;
		return skipped >= rows;
	}

	bool EndLine(bool add) {
		if (comment) {
			const bool counted = !starts_as_comment;
			comment = false;
			starts_as_comment = false;
			return counted && AddRow();
		}
		return add && AddRow();
	}

	const ReferenceGrammar &grammar;
	const idx_t rows;
	CSVState states[2] = {CSVState::NOT_SET, CSVState::NOT_SET};
	idx_t skipped = 0;
	idx_t lines = 0;
	idx_t pos = 0;
	bool comment = false;
	bool starts_as_comment = false;
};

struct FuzzDialect {
	string delimiter;
	char quote;
	char escape;
	char comment;
	NewLineIdentifier new_line;
	bool strict;
	idx_t columns;
};

string Describe(const FuzzDialect &dialect, const string &input) {
	string result = "delimiter=" + dialect.delimiter + " quote=" + to_string(int(dialect.quote)) +
	                " escape=" + to_string(int(dialect.escape)) + " comment=" + to_string(int(dialect.comment)) +
	                " new_line=" + to_string(int(dialect.new_line)) + " strict=" + to_string(dialect.strict) +
	                " columns=" + to_string(dialect.columns) + " input=";
	for (auto c : input) {
		if (c == '\n') {
			result += "\\n";
		} else if (c == '\r') {
			result += "\\r";
		} else if (c == '\\') {
			result += "\\\\";
		} else {
			result += c;
		}
	}
	return result;
}

FuzzDialect RandomDialect(std::mt19937_64 &rng) {
	static const char *delimiters[] = {",",  "|",  ";",   "\t", "||",   "ab", "\xc2\xa6",
	                                   "a|", ", ", "|~|", "::", "~|~|", "|||"};
	static const char quotes[] = {'"', '\'', '\0'};
	FuzzDialect dialect;
	dialect.delimiter = delimiters[rng() % (sizeof(delimiters) / sizeof(delimiters[0]))];
	dialect.quote = quotes[rng() % 3];
	if (dialect.quote == '\0') {
		dialect.escape = '\0';
	} else {
		const char escapes[] = {dialect.quote, '\\', '\0', '\''};
		dialect.escape = escapes[rng() % 4];
		if (dialect.escape == '\'' && dialect.quote == '\'') {
			dialect.escape = '\\';
		}
	}
	const char comments[] = {'\0', '#', '\0', '!'};
	dialect.comment = comments[rng() % 4];
	const NewLineIdentifier new_lines[] = {NewLineIdentifier::NOT_SET, NewLineIdentifier::SINGLE_N,
	                                       NewLineIdentifier::SINGLE_R, NewLineIdentifier::CARRY_ON};
	dialect.new_line = new_lines[rng() % 4];
	dialect.strict = rng() % 2;
	const idx_t columns[] = {1, 2, 3, 5};
	dialect.columns = columns[rng() % 4];
	return dialect;
}

string RandomInput(std::mt19937_64 &rng, const FuzzDialect &dialect) {
	string alphabet = dialect.delimiter + dialect.delimiter + "\n\n\r  aab";
	if (dialect.quote) {
		alphabet += string(3, dialect.quote);
	}
	if (dialect.escape) {
		alphabet += string(2, dialect.escape);
	}
	if (dialect.comment) {
		alphabet += dialect.comment;
	}
	const idx_t size = rng() % 4 == 0 ? rng() % 400 : rng() % 80;
	string input;
	for (idx_t i = 0; i < size; i++) {
		input += alphabet[rng() % alphabet.size()];
	}
	return input;
}

string RegularInput(std::mt19937_64 &rng, const FuzzDialect &dialect) {
	string newline = "\n";
	if (dialect.new_line == NewLineIdentifier::CARRY_ON ||
	    (dialect.new_line == NewLineIdentifier::NOT_SET && rng() % 2)) {
		newline = "\r\n";
	} else if (dialect.new_line == NewLineIdentifier::SINGLE_R) {
		newline = "\r";
	}
	const string plain = "ab1. ";
	string input;
	const idx_t rows = rng() % 12;
	for (idx_t row = 0; row < rows; row++) {
		if (rng() % 10 == 0) {
			input += newline;
		}
		if (dialect.comment && rng() % 6 == 0) {
			input += dialect.comment;
			input += rng() % 2 ? "a b" : "a" + dialect.delimiter + "b";
			input += newline;
		}
		const idx_t row_columns = rng() % 8 == 0 ? 1 + rng() % (dialect.columns + 2) : dialect.columns;
		for (idx_t column = 0; column < row_columns; column++) {
			if (column > 0) {
				input += dialect.delimiter;
			}
			const idx_t length = rng() % 6;
			string token;
			for (idx_t i = 0; i < length; i++) {
				token += plain[rng() % plain.size()];
			}
			if (dialect.quote && !token.empty() && token[0] != ' ' && rng() % 8 == 0) {
				token.insert(1 + rng() % token.size(), 1, dialect.quote);
			} else if (dialect.quote && rng() % 4 == 0) {
				string quoted(1, dialect.quote);
				quoted += token;
				const auto extra = rng() % 5;
				if (extra == 0) {
					quoted += dialect.delimiter;
				} else if (extra == 1) {
					quoted += newline;
				} else if (extra == 2 && dialect.escape == dialect.quote) {
					quoted += string(2, dialect.quote);
				} else if (extra == 2 && dialect.escape) {
					quoted += dialect.escape;
					quoted += rng() % 2 ? dialect.quote : dialect.escape;
				}
				quoted += dialect.quote;
				token = quoted;
				if (rng() % 4 == 0) {
					token = " " + token;
				}
				if (rng() % 4 == 0) {
					token += string(1 + rng() % 3, ' ');
				}
			}
			input += token;
		}
		if (row + 1 < rows || rng() % 2) {
			input += newline;
		}
	}
	return input;
}

FuzzDialect BlockDialect(std::mt19937_64 &rng) {
	static const char *delimiters[] = {",", "|", ";", "\t"};
	static const char quotes[] = {'"', '\'', '\0'};
	FuzzDialect dialect;
	dialect.delimiter = delimiters[rng() % 4];
	dialect.quote = quotes[rng() % 3];
	dialect.escape = rng() % 2 ? dialect.quote : '\0';
	dialect.comment = '\0';
	dialect.new_line = rng() % 2 ? NewLineIdentifier::NOT_SET : NewLineIdentifier::SINGLE_N;
	dialect.strict = rng() % 2;
	const idx_t columns[] = {1, 2, 3, 5, 16};
	dialect.columns = columns[rng() % 5];
	return dialect;
}

string BlockInput(std::mt19937_64 &rng, const FuzzDialect &dialect) {
	const string plain = "ab1. -x";
	const idx_t rarity = 20 + rng() % 400;
	const auto rare = [&]() {
		return rng() % rarity == 0;
	};
	string input;
	const idx_t rows = 20 + rng() % 300;
	for (idx_t row = 0; row < rows; row++) {
		if (rare()) {
			input += "\n";
		}
		const idx_t row_columns = rare() ? 1 + rng() % (dialect.columns + 2) : dialect.columns;
		for (idx_t column = 0; column < row_columns; column++) {
			if (column > 0) {
				input += dialect.delimiter;
			}
			string token;
			const idx_t length = rng() % 14;
			for (idx_t i = 0; i < length; i++) {
				token += plain[rng() % plain.size()];
			}
			if (dialect.quote && rng() % 3 == 0) {
				string quoted(1, dialect.quote);
				quoted += token;
				if (rare()) {
					quoted += dialect.delimiter;
				}
				if (rare()) {
					quoted += "\n";
				}
				if (rare()) {
					quoted += string(2, dialect.quote);
				}
				quoted += dialect.quote;
				token = quoted;
				if (rare()) {
					token = " " + token;
				}
				if (rare()) {
					token += " ";
				}
			} else if (dialect.quote && !token.empty() && rare()) {
				token.insert(rng() % token.size() + 1, 1, dialect.quote);
			}
			input += token;
		}
		if (row + 1 < rows || rng() % 2) {
			input += rare() ? "\r\n" : "\n";
		}
	}
	return input;
}

struct SplitResult {
	vector<string> rows;
	idx_t lines = 0;
	bool ever_quoted = false;
	bool ever_escaped = false;
	bool unstrict = false;
};

void Collect(const ReferenceSplitter &splitter, idx_t count, idx_t columns, SplitResult &result) {
	const auto &extras = splitter.extras;
	const auto &events = splitter.events;
	for (idx_t row = 0; row < count; row++) {
		auto &r = splitter.rows[row];
		string text = "row start=" + to_string(r.start) + " begin=" + to_string(r.begin) + " end=" + to_string(r.end) +
		              " last=" + to_string(r.last_position) + " lines=" + to_string(r.lines) +
		              " count=" + to_string(r.field_count) + " flags=" + to_string(r.flags) + " fields:";
		for (idx_t c = 0; c < MinValue<idx_t>(r.field_count, columns); c++) {
			auto &f = splitter.fields[c * splitter.capacity + row];
			text += " [" + to_string(f.start) + "," + to_string(f.offset) + "," + to_string(f.size) + "," +
			        to_string(f.flags) + "]";
		}
		for (auto &extra : extras) {
			if (extra.row == row) {
				text += " extra(" + to_string(extra.column) + ":" + to_string(extra.field.start) + "," +
				        to_string(extra.field.offset) + "," + to_string(extra.field.size) + "," +
				        to_string(extra.field.flags) + ")";
			}
		}
		for (auto &event : events) {
			if (event.row == row) {
				text += " event(" + to_string(event.column) + ":" + to_string(event.position) + "," +
				        to_string(int(event.kind)) + ")";
			}
		}
		result.rows.push_back(std::move(text));
	}
}

ReferenceGrammar FixedGrammar(const ReferenceGrammar &grammar) {
	auto fixed = grammar;
	if (grammar.delimiter_size > 1) {
		fixed.table[static_cast<uint8_t>(grammar.delimiter[0])][static_cast<uint8_t>(CSVState::ESCAPED_RETURN)] =
		    CSVState::DELIMITER_FIRST_BYTE;
	}
	return fixed;
}

SplitResult RunReference(const ReferenceGrammar &grammar_p, const FuzzDialect &dialect, const string &input,
                         idx_t max_rows, idx_t first_chunk, bool fixed = false) {
	SplitResult result;
	const auto grammar = fixed ? FixedGrammar(grammar_p) : grammar_p;
	ReferenceSplitter splitter(grammar, dialect.columns, max_rows);
	splitter.fixed = fixed;
	splitter.total = input.size();
	splitter.Reset(0, 0);
	const char *data = input.data();
	idx_t end = MinValue<idx_t>(first_chunk, input.size());
	bool last = end == input.size();
	for (idx_t guard = 0; guard < 100000; guard++) {
		const idx_t count = splitter.Split(data, 0, end, max_rows, last);
		Collect(splitter, count, dialect.columns, result);
		if (count > 0) {
			continue;
		}
		if (splitter.finished || last) {
			break;
		}
		end = input.size();
		last = true;
	}
	result.lines = splitter.lines_read;
	result.ever_quoted = splitter.ever_quoted;
	result.ever_escaped = splitter.ever_escaped;
	result.unstrict = splitter.used_unstrictness;
	return result;
}

string DescribeStart(const CsvRangeStart &start) {
	const auto state = start.states[1] == CSVState::QUOTED_NEW_LINE ? CSVState::QUOTED : start.states[1];
	return to_string(int(state)) + " line_end=" + to_string(int64_t(start.line_end)) +
	       " last=" + to_string(int64_t(start.last_position));
}

RowDialect LeanDialect(const CsvGrammar &grammar, idx_t columns) {
	auto dialect = RowDialect::DuckDB(grammar);
	dialect.empty_lines = columns == 1;
	return dialect;
}

string RunLean(const ReferenceGrammar &grammar, const FuzzDialect &dialect, const string &input, idx_t piece_blocks,
               idx_t max_line_size, bool pad_rows, bool &accepted, bool &padded, bool &irregular, bool settled = true) {
	accepted = false;
	padded = false;
	irregular = false;
	RowLexer lean(LeanDialect(grammar, dialect.columns), dialect.columns, max_line_size, true, pad_rows, true);
	auto exact = RunReference(grammar, dialect, input, input.size() + 2, input.size(), true);
	RowCursor cursor;
	RowFields fields;
	lean.Start(cursor, 0, settled);
	cursor.file_start = true;
	const char *data = input.data();
	while (cursor.ok && cursor.pos < input.size()) {
		const idx_t end = MinValue<idx_t>(cursor.pos + piece_blocks * 64 + lean.Lookahead(), input.size());
		const bool last = end == input.size();
		if (!lean.Feed(cursor, data, 0, end, last, &fields)) {
			return "lexer refused the input at " + to_string(cursor.pos) + "\n";
		}
	}
	CsvRangeEntry entry;
	if (!lean.Finish(cursor, data, 0, input.size(), true, &fields, CSVState::NOT_SET, entry)) {
		return "lexer refused the end of the input\n";
	}
	accepted = true;
	padded = cursor.padded;
	irregular = !fields.anomalies.empty() || cursor.extras_dropped;
	const idx_t columns = dialect.columns;
	string mismatch;
	if (cursor.rows != exact.rows.size()) {
		mismatch += "rows " + to_string(cursor.rows) + " != " + to_string(exact.rows.size()) + "\n";
	}
	if (cursor.lines != exact.lines) {
		mismatch += "lines " + to_string(cursor.lines) + " != " + to_string(exact.lines) + "\n";
	}
	if (fields.end_count != cursor.rows * columns || fields.row_count < cursor.rows) {
		mismatch += "field counts " + to_string(fields.end_count) + " " + to_string(fields.row_count) + "\n";
		return mismatch;
	}
	bool short_rows = false;
	bool unexplained_short = false;
	for (idx_t row = 0; row < cursor.rows && row < exact.rows.size(); row++) {
		string text = "row start=" + to_string(fields.rows[row]) + " fields:";
		bool padding = false;
		for (idx_t column = 0; column < columns; column++) {
			idx_t begin = column == 0
			                  ? fields.rows[row]
			                  : (fields.ends[row * columns + column - 1] & ROW_FIELD_MASK) + grammar.delimiter_size;
			const uint32_t end_bits = fields.ends[row * columns + column];
			idx_t end = end_bits & ROW_FIELD_MASK;
			const bool pad = column > 0 && (end_bits & ROW_FIELD_FLAG) && begin > end;
			if (padding && !pad) {
				text += " field after padding";
			}
			padding = padding || pad;
			short_rows = short_rows || pad;
			if (pad) {
				continue;
			}
			for (idx_t at = begin; at < end && !(end_bits & ROW_FIELD_FLAG); at++) {
				if ((grammar.quote && input[at] == grammar.quote) || (grammar.escape && input[at] == grammar.escape)) {
					text += " unflagged";
					break;
				}
			}
			const bool spaced = grammar.quote && grammar.trim_quoted_spaces;
			idx_t open = begin;
			if (spaced && end - open > 1 && input[open] == ' ' && input[open + 1] == grammar.quote) {
				open++;
			}
			const bool quoted = grammar.quote && end > open && input[open] == grammar.quote;
			if (quoted) {
				idx_t close = end;
				while (spaced && close > open + 1 && input[close - 1] == ' ') {
					close--;
				}
				begin = open + 1;
				end = MaxValue(close - 1, begin);
			}
			text += " [" + to_string(begin) + "," + to_string(end - begin) + "," + to_string(quoted) + "]";
		}
		const auto &expected = exact.rows[row];
		string reduced =
		    "row start=" + expected.substr(expected.find("start=") + 6,
		                                   expected.find(' ', expected.find("start=")) - expected.find("start=") - 6);
		reduced += " fields:";
		auto at = expected.find("fields:");
		at = at == string::npos ? expected.size() : at + 7;
		const auto reduced_size = reduced.size();
		while ((at = expected.find('[', at)) != string::npos) {
			const auto close = expected.find(']', at);
			const auto parts = StringUtil::Split(expected.substr(at + 1, close - at - 1), ',');
			const auto flags = std::stoull(parts[3]);
			reduced += " [" + parts[1] + "," + parts[2] + "," + to_string((flags & CSV_FIELD_QUOTED) != 0) + "]";
			at = close;
		}
		if (columns == 1 && reduced.size() == reduced_size) {
			reduced += " [" + to_string(fields.rows[row]) + ",0,0]";
		}
		for (auto &event : fields.events) {
			if (event.row != row) {
				continue;
			}
			const idx_t position = event.position;
			const bool quoted = grammar.quote && position < input.size() &&
			                    (input[position] == grammar.quote ||
			                     (grammar.empty_space && input[position] == ' ' && position + 1 < input.size() &&
			                      input[position + 1] == grammar.quote));
			text +=
			    " event(" + to_string(event.column) + ":" + to_string(position) + "," + to_string(quoted ? 0 : 1) + ")";
		}
		for (at = expected.find("event("); at != string::npos; at = expected.find("event(", at + 1)) {
			reduced += " " + expected.substr(at, expected.find(')', at) - at + 1);
		}
		const auto flags_at = expected.find(" flags=") + 7;
		const auto row_flags = std::stoull(expected.substr(flags_at, expected.find(' ', flags_at) - flags_at));
		if ((row_flags & CSV_ROW_UNFINISHED) != 0) {
			reduced =
			    reduced.substr(0, reduced.find(" fields:")) +
			    reduced.substr(reduced.find(" event(") == string::npos ? reduced.size() : reduced.find(" event(")) +
			    " unfinished";
		}
		if (row + 1 == cursor.rows && cursor.unfinished) {
			text = text.substr(0, text.find(" fields:")) +
			       text.substr(text.find(" event(") == string::npos ? text.size() : text.find(" event(")) +
			       " unfinished";
		}
		if (text != reduced) {
			mismatch += "lean " + text + "\nexact " + reduced + "\n";
		}
		const auto count_at = expected.find(" count=") + 7;
		idx_t exact_fields = std::stoull(expected.substr(count_at, expected.find(' ', count_at) - count_at));
		if (columns == 1 && exact_fields == 0) {
			exact_fields = 1;
		}
		const auto anomaly = fields.Anomaly(row);
		const bool unfinished_row = (row_flags & CSV_ROW_UNFINISHED) != 0;
		if (unfinished_row) {
			continue;
		}
		const bool expect_anomaly =
		    (exact_fields < columns && !pad_rows) || (exact_fields > columns && grammar.strict_mode);
		if (expect_anomaly != (anomaly != nullptr) || (anomaly && anomaly->fields != exact_fields)) {
			mismatch += "anomaly row " + to_string(row) + " exact fields " + to_string(exact_fields) + " lean " +
			            (anomaly ? to_string(anomaly->fields) : string("none")) + "\n";
		}
		unexplained_short = unexplained_short || (padding && !pad_rows && !anomaly);
	}
	if (short_rows != cursor.padded || unexplained_short) {
		mismatch += "padded " + to_string(cursor.padded) + " short rows " + to_string(short_rows) + "\n";
	}
	CsvRangeTransfer transfers;
	ReferenceTransfer(FixedGrammar(grammar), data, 0, 0, input.size(), transfers, true);
	auto transfer = transfers[static_cast<uint8_t>(CSVState::NOT_SET)];
	const idx_t boundary = transfer.first_boundary;
	if (grammar.carry_on_newline && boundary != DConstants::INVALID_INDEX && boundary < input.size() &&
	    input[boundary - 1] == '\r' && input[boundary] == '\n') {
		transfer.first_boundary++;
	}
	if (!cursor.unterminated &&
	    (DescribeStart(transfer.end) != DescribeStart(entry.end) || transfer.first_boundary != entry.first_boundary)) {
		mismatch += "entry end " + DescribeStart(entry.end) + " first=" + to_string(int64_t(entry.first_boundary)) +
		            " != " + DescribeStart(transfer.end) + " first=" + to_string(int64_t(transfer.first_boundary)) +
		            "\n";
	}
	return mismatch;
}

struct ReferenceCounts {
	vector<RowCount> rows;
	bool ever_quoted = false;
	bool ever_escaped = false;
	bool used_unstrictness = false;
	bool invalid = false;
	bool unterminated = false;
};

ReferenceCounts CountReference(const ReferenceGrammar &grammar, const string &input) {
	ReferenceCounts result;
	const auto &table = grammar.table;
	CSVState states[2] = {CSVState::NOT_SET, CSVState::NOT_SET};
	idx_t delimiters = 0;
	idx_t empty_lines = 0;
	idx_t line_start = 0;
	bool comment = false;
	bool starts_as_comment = false;
	auto add_row = [&](idx_t position, uint8_t flags) {
		if ((states[0] == CSVState::DELIMITER &&
		     (states[1] == CSVState::RECORD_SEPARATOR || states[1] == CSVState::CARRIAGE_RETURN ||
		      states[1] == CSVState::DELIMITER)) ||
		    (states[0] == CSVState::STANDARD && states[1] == CSVState::DELIMITER)) {
			flags |= ROW_COUNT_EMPTY_LAST;
		}
		const idx_t end = position == DConstants::INVALID_INDEX ? position : position + 1;
		result.rows.push_back({delimiters + 1, empty_lines, line_start, end, flags});
		delimiters = 0;
		line_start = end;
	};
	for (idx_t pos = 0; pos < input.size(); pos++) {
		const char byte = input[pos];
		states[0] = states[1];
		states[1] = table.state_machine[static_cast<uint8_t>(byte)][static_cast<uint8_t>(states[1])];
		switch (states[1]) {
		case CSVState::INVALID:
			result.invalid = true;
			return result;
		case CSVState::RECORD_SEPARATOR:
		case CSVState::CARRIAGE_RETURN:
			if (states[0] == CSVState::RECORD_SEPARATOR || states[0] == CSVState::NOT_SET) {
				empty_lines++;
				line_start = pos + 1;
			} else if (states[0] != CSVState::CARRIAGE_RETURN) {
				if (comment) {
					add_row(pos, starts_as_comment ? ROW_COUNT_COMMENT : ROW_COUNT_MID_COMMENT);
					comment = false;
					starts_as_comment = false;
				} else {
					add_row(pos, 0);
				}
			}
			break;
		case CSVState::DELIMITER:
			delimiters++;
			break;
		case CSVState::QUOTED:
			if ((states[0] == CSVState::UNQUOTED || states[0] == CSVState::MAYBE_QUOTED) && grammar.escape != '\0') {
				result.ever_escaped = true;
			}
			if ((states[0] == CSVState::ESCAPE || states[0] == CSVState::ESCAPED_RETURN ||
			     states[0] == CSVState::UNQUOTED_ESCAPE) &&
			    (byte == grammar.quote || !grammar.strict_mode)) {
				result.ever_escaped = true;
				result.used_unstrictness = result.used_unstrictness || states[0] == CSVState::UNQUOTED_ESCAPE;
			}
			result.ever_quoted = true;
			break;
		case CSVState::UNQUOTED:
			result.ever_escaped = result.ever_escaped || states[0] == CSVState::MAYBE_QUOTED;
			break;
		case CSVState::UNQUOTED_ESCAPE:
			result.used_unstrictness = true;
			break;
		case CSVState::COMMENT:
			if (states[0] != CSVState::COMMENT) {
				starts_as_comment = states[0] != CSVState::STANDARD && states[0] != CSVState::DELIMITER;
				comment = true;
			}
			break;
		default:
			break;
		}
	}
	if ((states[1] == CSVState::QUOTED || states[1] == CSVState::QUOTED_NEW_LINE) && states[0] != CSVState::UNQUOTED) {
		result.unterminated = true;
		return result;
	}
	if (states[1] == CSVState::RECORD_SEPARATOR || states[1] == CSVState::CARRIAGE_RETURN ||
	    states[1] == CSVState::NOT_SET) {
		return result;
	}
	add_row(DConstants::INVALID_INDEX, comment ? (starts_as_comment ? ROW_COUNT_COMMENT : ROW_COUNT_MID_COMMENT) : 0);
	return result;
}

string DescribeCount(const RowCount &row) {
	return "[" + to_string(row.columns) + "," + to_string(row.empty_lines) + "," + to_string(row.begin) + "," +
	       to_string(int64_t(row.end)) + "," + to_string(row.flags) + "]";
}

struct CountCoverage {
	idx_t rows = 0;
	idx_t quoted = 0;
	idx_t comments = 0;
	idx_t invalid = 0;
	idx_t unterminated = 0;
};

string CountMismatch(const ReferenceGrammar &grammar, const string &input, std::mt19937_64 &rng,
                     CountCoverage &coverage) {
	const auto expected = CountReference(FixedGrammar(grammar), input);
	coverage.rows += expected.rows.size();
	coverage.quoted += expected.ever_quoted;
	coverage.invalid += expected.invalid;
	coverage.unterminated += expected.unterminated;
	for (auto &row : expected.rows) {
		coverage.comments += (row.flags & (ROW_COUNT_COMMENT | ROW_COUNT_MID_COMMENT)) != 0;
	}
	RowLexer lexer(RowDialect::DuckDB(grammar), 1, idx_t(1) << 20, false);
	RowCursor cursor;
	lexer.Start(cursor, 0, true, false);
	cursor.file_start = true;
	vector<RowCount> rows;
	while (cursor.pos < input.size() && !cursor.invalid) {
		idx_t end = MinValue<idx_t>(input.size(), cursor.pos + 1 + rng() % 150);
		const idx_t before = cursor.pos;
		const idx_t limit = rows.size() + 1 + rng() % 4;
		RowShapes shapes {rows, limit};
		lexer.CountRows(cursor, input.data(), 0, end, end == input.size(), shapes);
		if (cursor.pos == before && rows.size() < limit && !cursor.invalid) {
			end = input.size();
			RowShapes rest {rows, limit};
			lexer.CountRows(cursor, input.data(), 0, end, true, rest);
		}
	}
	lexer.FinishCount(cursor, rows);
	string mismatch;
	if (cursor.invalid != expected.invalid || cursor.unterminated != expected.unterminated ||
	    cursor.ever_quoted != expected.ever_quoted || cursor.ever_escaped != expected.ever_escaped ||
	    cursor.unstrict != expected.used_unstrictness) {
		mismatch += "flags invalid " + to_string(cursor.invalid) + "/" + to_string(expected.invalid) +
		            " unterminated " + to_string(cursor.unterminated) + "/" + to_string(expected.unterminated) +
		            " quoted " + to_string(cursor.ever_quoted) + "/" + to_string(expected.ever_quoted) + " escaped " +
		            to_string(cursor.ever_escaped) + "/" + to_string(expected.ever_escaped) + " unstrict " +
		            to_string(cursor.unstrict) + "/" + to_string(expected.used_unstrictness) + "\n";
	}
	string actual_rows;
	string expected_rows;
	for (auto &row : rows) {
		actual_rows += DescribeCount(row);
	}
	for (auto &row : expected.rows) {
		expected_rows += DescribeCount(row);
	}
	if (actual_rows != expected_rows) {
		mismatch += "rows\n  lexer " + actual_rows + "\n  table " + expected_rows + "\n";
	}
	return mismatch;
}

} // namespace

TEST_CASE("CSV splitter, transfer and skip match the byte state machine", "[csv]") {
	CSVReaderOptions options;
	const char *seed = getenv("CSV_FUZZ_SEED");
	const char *iterations = getenv("CSV_FUZZ_ITERATIONS");
	std::mt19937_64 rng(seed ? std::stoull(seed) : 20261118);
	const idx_t count = iterations ? std::stoull(iterations) : 100000;
	idx_t lean_accepted = 0;
	idx_t comment_accepted = 0;
	idx_t wide_accepted = 0;
	idx_t padded_accepted = 0;
	idx_t irregular_accepted = 0;
	idx_t spaced_accepted = 0;
	idx_t literal_accepted = 0;
	CountCoverage coverage;
	for (idx_t iteration = 0; iteration < count; iteration++) {
		const auto dialect = RandomDialect(rng);
		CSVStateMachineOptions machine_options(dialect.delimiter, dialect.quote, dialect.escape, dialect.comment,
		                                       dialect.new_line, dialect.strict);
		CSVStateMachine machine(options, machine_options);
		const auto grammar = MakeReferenceGrammar(machine);
		const auto input = rng() % 2 ? RandomInput(rng, dialect) : RegularInput(rng, dialect);

		const idx_t max_line_sizes[] = {64, 1000, 1 << 20};
		const idx_t max_line_size = max_line_sizes[rng() % 3];
		const idx_t piece_blocks = 1 + rng() % 3;
		const bool pad_rows = rng() % 2;
		bool accepted = false;
		bool padded = false;
		bool irregular = false;
		const auto lean_mismatch =
		    RunLean(grammar, dialect, input, piece_blocks, max_line_size, pad_rows, accepted, padded, irregular);
		lean_accepted += accepted;
		padded_accepted += padded;
		irregular_accepted += accepted && irregular;
		comment_accepted += accepted && dialect.comment && input.find(dialect.comment) != string::npos;
		wide_accepted += accepted && dialect.delimiter.size() > 1 && input.find(dialect.delimiter) != string::npos;
		literal_accepted += accepted && dialect.quote &&
		                    (input.find(string("a") + dialect.quote) != string::npos ||
		                     input.find(string("b") + dialect.quote) != string::npos ||
		                     input.find(string("1") + dialect.quote) != string::npos);
		spaced_accepted += accepted && dialect.quote &&
		                   (input.find(string(" ") + dialect.quote) != string::npos ||
		                    input.find(string(1, dialect.quote) + " ") != string::npos);
		if (!lean_mismatch.empty()) {
			if (const char *dump = getenv("CSV_FUZZ_DUMP")) {
				std::ofstream(dump, std::ios::binary) << input;
			}
			FAIL("lean mismatch: " + Describe(dialect, input) + " pieces=" + to_string(piece_blocks) +
			     " max_line_size=" + to_string(max_line_size) + " pad_rows=" + to_string(pad_rows) + "\n" +
			     lean_mismatch);
		}
		const auto count_mismatch = CountMismatch(grammar, input, rng, coverage);
		if (!count_mismatch.empty()) {
			if (const char *dump = getenv("CSV_FUZZ_DUMP")) {
				std::ofstream(dump, std::ios::binary) << input;
			}
			FAIL("count mismatch: " + Describe(dialect, input) + "\n" + count_mismatch);
		}
		if (dialect.quote && !grammar.carry_on_newline && !input.empty()) {
			RowLexer quoted_lexer(LeanDialect(grammar, dialect.columns), dialect.columns, max_line_size, true);
			RowCursor quoted_cursor;
			quoted_lexer.Start(quoted_cursor, 0, true, false);
			quoted_cursor.in_quotes = 1;
			CsvRangeEntry quoted_entry;
			if (quoted_lexer.Feed(quoted_cursor, input.data(), 0, input.size(), true, nullptr) &&
			    quoted_lexer.Finish(quoted_cursor, input.data(), 0, input.size(), true, nullptr, CSVState::QUOTED,
			                        quoted_entry) &&
			    !quoted_cursor.unterminated) {
				CsvRangeTransfer reference;
				ReferenceTransfer(FixedGrammar(grammar), input.data(), 0, 0, input.size(), reference, true);
				const auto &exact = reference[static_cast<uint8_t>(CSVState::QUOTED)];
				if (DescribeStart(exact.end) != DescribeStart(quoted_entry.end) ||
				    exact.first_boundary != quoted_entry.first_boundary) {
					FAIL("quoted lean mismatch: " + Describe(dialect, input) + "\nlean " +
					     DescribeStart(quoted_entry.end) + " first=" + to_string(int64_t(quoted_entry.first_boundary)) +
					     "\nexact " + DescribeStart(exact.end) + " first=" + to_string(int64_t(exact.first_boundary)));
				}
			}
		}

		options.dialect_options.state_machine_options.new_line = CSVOption<NewLineIdentifier>(dialect.new_line);
		const idx_t skip_rows = 1 + rng() % 3;
		const auto skip_grammar = FixedGrammar(grammar);
		ReferenceSkip reference_skip(skip_grammar, skip_rows);
		const bool reference_done = reference_skip.Feed(input.data(), 0, input.size());
		RowSkip new_skip(RowDialect::DuckDB(grammar), skip_rows);
		bool new_done = input.empty() && new_skip.Feed(input.data(), 0, 0, true);
		for (idx_t offset = 0; offset < input.size() && !new_done;) {
			const idx_t chunk = MinValue<idx_t>(1 + rng() % 70, input.size() - offset);
			new_done = new_skip.Feed(input.data() + offset, offset, offset + chunk, offset + chunk == input.size());
			offset += chunk;
		}
		const idx_t expected_start = reference_skip.Start(input);
		if (reference_done != new_done ||
		    (reference_done && (expected_start != new_skip.Start() || reference_skip.lines != new_skip.Lines()))) {
			if (const char *dump = getenv("CSV_FUZZ_DUMP")) {
				std::ofstream(dump, std::ios::binary) << input;
			}
			FAIL("skip mismatch: " + Describe(dialect, input) + " rows=" + to_string(skip_rows) +
			     " expected start=" + to_string(expected_start) + " lines=" + to_string(reference_skip.lines) +
			     " done=" + to_string(reference_done) + " actual start=" + to_string(int64_t(new_skip.Start())) +
			     " lines=" + to_string(new_skip.Lines()) + " done=" + to_string(new_done));
		}
	}
	REQUIRE(lean_accepted > count / 50);
	REQUIRE(comment_accepted > count / 50);
	REQUIRE(wide_accepted > count / 50);
	REQUIRE(padded_accepted > count / 100);
	REQUIRE(irregular_accepted > count / 100);
	REQUIRE(spaced_accepted > count / 50);
	REQUIRE(literal_accepted > count / 100);
	REQUIRE(coverage.rows > count);
	REQUIRE(coverage.quoted > count / 10);
	REQUIRE(coverage.comments > count / 10);
	REQUIRE(coverage.invalid > count / 10);
	REQUIRE(coverage.unterminated > count / 100);
}

namespace {

struct ReferenceRow {
	idx_t start = 0;
	vector<std::tuple<idx_t, idx_t, bool>> fields;
};

bool ReferencePostgresRows(const RowDialect &dialect, const string &input, vector<ReferenceRow> &rows, idx_t &lines) {
	rows.clear();
	lines = 0;
	const bool anywhere = dialect.quoting == RowQuoting::ANYWHERE;
	bool in_quotes = false;
	bool escaped_next = false;
	bool open = false;
	bool flagged = false;
	idx_t field_begin = 0;
	idx_t plain_cr = DConstants::INVALID_INDEX;
	ReferenceRow row;
	for (idx_t i = 0; i < input.size(); i++) {
		const char ch = input[i];
		if (!open) {
			row = ReferenceRow();
			row.start = i;
			field_begin = i;
			flagged = false;
			open = true;
		}
		if ((dialect.backslash || (anywhere && dialect.escape_in_quotes)) && ch == dialect.escape) {
			flagged = true;
		}
		if (anywhere && ch == dialect.quote) {
			flagged = true;
		}
		if (escaped_next) {
			escaped_next = false;
			continue;
		}
		if (dialect.backslash && ch == dialect.escape) {
			escaped_next = true;
			continue;
		}
		if (anywhere) {
			if (in_quotes && dialect.escape_in_quotes && ch == dialect.escape) {
				escaped_next = true;
				continue;
			}
			if (ch == dialect.quote) {
				in_quotes = !in_quotes;
				continue;
			}
		}
		if (in_quotes) {
			continue;
		}
		if (ch == dialect.delimiter) {
			row.fields.emplace_back(field_begin, i, flagged);
			field_begin = i + 1;
			flagged = false;
			continue;
		}
		if (ch == dialect.line_break) {
			idx_t end = i;
			if (dialect.line_break == '\n' && plain_cr == i - 1 && i > 0) {
				end--;
			}
			row.fields.emplace_back(field_begin, end, flagged);
			rows.push_back(row);
			lines++;
			open = false;
			continue;
		}
		if (ch == '\r') {
			plain_cr = i;
		}
	}
	if (in_quotes || (escaped_next && !dialect.backslash)) {
		return false;
	}
	if (open) {
		const bool stripped = dialect.line_break == '\n' && !input.empty() && plain_cr == input.size() - 1;
		const idx_t end = stripped ? input.size() - 1 : input.size();
		row.fields.emplace_back(field_begin, end, flagged);
		rows.push_back(row);
		lines++;
	}
	if (dialect.physical_lines) {
		lines = NumericCast<idx_t>(std::count(input.begin(), input.end(), dialect.line_break)) + (open ? 1 : 0);
	}
	return true;
}

string PostgresInput(std::mt19937_64 &rng, const RowDialect &dialect) {
	string alphabet = string(2, dialect.delimiter) + string(2, dialect.line_break) + "\n\r ab";
	if (dialect.quote) {
		alphabet += string(2, dialect.quote);
	}
	if (dialect.escape) {
		alphabet += string(2, dialect.escape);
	}
	const idx_t size = rng() % 4 == 0 ? rng() % 300 : rng() % 70;
	string input;
	for (idx_t i = 0; i < size; i++) {
		input += alphabet[rng() % alphabet.size()];
	}
	return input;
}

string BlockAlignedInput(std::mt19937_64 &rng, const FuzzDialect &dialect, idx_t size, const string &ending) {
	const string fields[] = {"a", "ab", "", " ", "1"};
	string input;
	while (input.size() + ending.size() < size) {
		const auto &field = fields[rng() % 5];
		if (dialect.quote && rng() % 3 == 0) {
			input += dialect.quote + field + dialect.quote;
		} else {
			input += field;
		}
		input += rng() % 3 ? dialect.delimiter : "\n";
	}
	input.resize(size - ending.size());
	return input + ending;
}

} // namespace

TEST_CASE("Row counts of inputs that end at a block boundary match the byte state machine", "[csv]") {
	CSVReaderOptions options;
	std::mt19937_64 rng(20261122);
	idx_t trailing_empty = 0;
	for (const char *delimiter : {",", "|", "\t"}) {
		for (const char quote : {'"', '\'', '\0'}) {
			for (const char escape : {'\0', quote, '\\'}) {
				for (const bool strict : {false, true}) {
					for (const auto new_line : {NewLineIdentifier::NOT_SET, NewLineIdentifier::SINGLE_N}) {
						for (const char comment : {'\0', '#'}) {
							if (!quote && escape) {
								continue;
							}
							const FuzzDialect dialect {delimiter, quote, escape, comment, new_line, strict, 1};
							CSVStateMachineOptions machine_options(dialect.delimiter, dialect.quote, dialect.escape,
							                                       dialect.comment, dialect.new_line, dialect.strict);
							CSVStateMachine machine(options, machine_options);
							const auto grammar = MakeReferenceGrammar(machine);
							const string quoted = quote ? string(1, quote) + "a" + string(1, quote) : string("a");
							const string endings[] = {"",
							                          delimiter,
							                          string(delimiter) + delimiter,
							                          quoted + delimiter,
							                          string(" ") + delimiter,
							                          string("a") + delimiter};
							for (const auto &ending : endings) {
								for (idx_t blocks = 1; blocks <= 3; blocks++) {
									const auto input =
									    BlockAlignedInput(rng, dialect, blocks * CSV_BLOCK_BYTES, ending);
									const auto expected = CountReference(FixedGrammar(grammar), input);
									for (const idx_t piece : {CSV_BLOCK_BYTES, input.size()}) {
										RowLexer lexer(RowDialect::DuckDB(grammar), 1, idx_t(1) << 20, false);
										RowCursor cursor;
										lexer.Start(cursor, 0, true, false);
										cursor.file_start = true;
										vector<RowCount> rows;
										for (idx_t end = piece; !cursor.invalid;
										     end = MinValue(end + piece, input.size())) {
											RowShapes shapes {rows, NumericLimits<idx_t>::Maximum()};
											lexer.CountRows(cursor, input.data(), 0, end, end == input.size(), shapes);
											if (end == input.size()) {
												break;
											}
										}
										trailing_empty += cursor.trailing_empty;
										lexer.FinishCount(cursor, rows);
										string actual_rows;
										string expected_rows;
										for (auto &row : rows) {
											actual_rows += DescribeCount(row);
										}
										for (auto &row : expected.rows) {
											expected_rows += DescribeCount(row);
										}
										INFO(Describe(dialect, input) << " piece=" << piece);
										REQUIRE(actual_rows == expected_rows);
										REQUIRE(cursor.invalid == expected.invalid);
										REQUIRE(cursor.unterminated == expected.unterminated);
										REQUIRE(cursor.ever_quoted == expected.ever_quoted);
										REQUIRE(cursor.ever_escaped == expected.ever_escaped);
									}
								}
							}
						}
					}
				}
			}
		}
	}
	REQUIRE(trailing_empty > 0);
}

TEST_CASE("Row lexer reads long regular inputs like the byte state machine", "[csv]") {
	CSVReaderOptions options;
	const char *seed = getenv("CSV_FUZZ_SEED");
	const char *iterations = getenv("CSV_FUZZ_ITERATIONS");
	std::mt19937_64 rng(seed ? std::stoull(seed) : 20261119);
	const idx_t count = iterations ? std::stoull(iterations) / 10 : 10000;
	idx_t accepted_count = 0;
	idx_t counted = 0;
	idx_t unsettled = 0;
	for (idx_t iteration = 0; iteration < count; iteration++) {
		const auto dialect = BlockDialect(rng);
		CSVStateMachineOptions machine_options(dialect.delimiter, dialect.quote, dialect.escape, dialect.comment,
		                                       dialect.new_line, dialect.strict);
		CSVStateMachine machine(options, machine_options);
		const auto grammar = MakeReferenceGrammar(machine);
		const auto input = BlockInput(rng, dialect);
		const idx_t max_line_sizes[] = {64, 1000, 1 << 20};
		const idx_t max_line_size = max_line_sizes[rng() % 3];
		const idx_t piece_blocks = 1 + rng() % 16;
		const bool pad_rows = rng() % 2;
		const auto fail = [&](const string &what, const string &text, const string &mismatch) {
			if (const char *dump = getenv("CSV_FUZZ_DUMP")) {
				std::ofstream(dump, std::ios::binary) << text;
			}
			FAIL(what + ": " + Describe(dialect, text) + " pieces=" + to_string(piece_blocks) +
			     " max_line_size=" + to_string(max_line_size) + " pad_rows=" + to_string(pad_rows) + "\n" + mismatch);
		};
		bool accepted = false;
		bool padded = false;
		bool irregular = false;
		const auto mismatch =
		    RunLean(grammar, dialect, input, piece_blocks, max_line_size, pad_rows, accepted, padded, irregular);
		if (!mismatch.empty()) {
			fail("block mismatch", input, mismatch);
		}
		accepted_count += accepted;
		const auto exact = RunReference(grammar, dialect, input, input.size() + 2, input.size(), true);
		RowLexer counter(LeanDialect(grammar, dialect.columns), dialect.columns, max_line_size, true, pad_rows);
		RowCursor cursor;
		counter.Start(cursor, 0, true);
		cursor.file_start = true;
		bool ok = true;
		while (ok && cursor.pos < input.size()) {
			const idx_t end = MinValue<idx_t>(cursor.pos + piece_blocks * 64 + counter.Lookahead(), input.size());
			ok = counter.Feed(cursor, input.data(), 0, end, end == input.size(), nullptr);
		}
		CsvRangeEntry entry;
		if (ok && counter.Finish(cursor, input.data(), 0, input.size(), true, nullptr, CSVState::NOT_SET, entry)) {
			if (cursor.rows != exact.rows.size() || cursor.lines != exact.lines) {
				fail("count mismatch", input,
				     "rows " + to_string(cursor.rows) + " != " + to_string(exact.rows.size()) + ", lines " +
				         to_string(cursor.lines) + " != " + to_string(exact.lines));
			}
			counted++;
		}
		if (exact.rows.size() < 2) {
			continue;
		}
		const auto &row = exact.rows[1 + rng() % (exact.rows.size() - 1)];
		const auto suffix = input.substr(std::stoull(row.substr(row.find("start=") + 6)));
		const auto unsettled_mismatch = RunLean(grammar, dialect, suffix, piece_blocks, max_line_size, pad_rows,
		                                        accepted, padded, irregular, false);
		if (accepted && !unsettled_mismatch.empty()) {
			fail("unsettled mismatch", suffix, unsettled_mismatch);
		}
		unsettled += accepted;
	}
	REQUIRE(accepted_count > count / 2);
	REQUIRE(counted > count / 20);
	REQUIRE(unsettled > count / 20);
}

TEST_CASE("Row lexer splits PostgreSQL csv and text like the scalar rules", "[csv]") {
	const char *seed = getenv("CSV_FUZZ_SEED");
	const char *iterations = getenv("CSV_FUZZ_ITERATIONS");
	std::mt19937_64 rng(seed ? std::stoull(seed) : 20261119);
	const idx_t count = iterations ? std::stoull(iterations) : 20000;
	idx_t accepted = 0;
	for (idx_t iteration = 0; iteration < count; iteration++) {
		const char delimiters[] = {',', '|', '\t'};
		const char delimiter = delimiters[rng() % 3];
		const char line_break = rng() % 2 ? '\n' : '\r';
		RowDialect dialect;
		if (rng() % 2) {
			dialect = RowDialect::PostgresText(delimiter, line_break);
		} else {
			const char quote = rng() % 2 ? '"' : '\'';
			dialect = RowDialect::PostgresCsv(delimiter, quote, rng() % 2 ? quote : '\\', line_break);
		}
		const auto input = PostgresInput(rng, dialect);
		vector<ReferenceRow> expected;
		idx_t expected_lines = 0;
		const bool terminated = ReferencePostgresRows(dialect, input, expected, expected_lines);
		const idx_t columns = 1 + rng() % 3;
		const bool check_arity = rng() % 2;
		RowLexer lexer(dialect, columns, idx_t(1) << 20, true);
		RowCursor cursor;
		RowFields fields;
		lexer.Start(cursor, 0, true, check_arity);
		const idx_t piece = 64 * (1 + rng() % 3);
		bool ok = true;
		while (ok && cursor.pos < input.size()) {
			const idx_t end = MinValue<idx_t>(cursor.pos + piece, input.size());
			ok = lexer.Feed(cursor, input.data(), 0, end, end == input.size(), &fields);
		}
		CsvRangeEntry entry;
		ok = ok && lexer.Finish(cursor, input.data(), 0, input.size(), true, &fields, CSVState::NOT_SET, entry);
		if (!ok) {
			continue;
		}
		accepted++;
		string shown;
		for (auto ch : input) {
			if (ch == '\n') {
				shown += "<LF>";
			} else if (ch == '\r') {
				shown += "<CR>";
			} else if (ch == '\t') {
				shown += "<TAB>";
			} else {
				shown += ch;
			}
		}
		const string description =
		    string("dialect text=") + to_string(dialect.backslash) + " delimiter=" + to_string(int(dialect.delimiter)) +
		    " quote=" + to_string(int(dialect.quote)) + " escape=" + to_string(int(dialect.escape)) +
		    " columns=" + to_string(columns) + " arity=" + to_string(check_arity) + " piece=" + to_string(piece) +
		    " input=" + shown;
		if (terminated == cursor.unfinished) {
			FAIL("unterminated input not marked as an unfinished row, or the other way around: " + description);
		}
		if (!terminated) {
			continue;
		}
		if (cursor.rows != expected.size() || cursor.lines != expected_lines) {
			FAIL("row count mismatch: " + description + " rows " + to_string(cursor.rows) + " vs " +
			     to_string(expected.size()) + " lines " + to_string(cursor.lines) + " vs " + to_string(expected_lines));
		}
		idx_t field = 0;
		for (idx_t row = 0; row < expected.size(); row++) {
			if (fields.rows[row] != expected[row].start) {
				FAIL("row start mismatch at row " + to_string(row) + ": " + description);
			}
			idx_t begin = expected[row].start;
			for (auto &expected_field : expected[row].fields) {
				if (field >= fields.end_count) {
					FAIL("missing field: " + description);
				}
				const uint32_t end_bits = fields.ends[field++];
				const idx_t end = end_bits & ROW_FIELD_MASK;
				const bool flag = (end_bits & ROW_FIELD_FLAG) != 0;
				if (begin != std::get<0>(expected_field) || end != std::get<1>(expected_field) ||
				    flag != std::get<2>(expected_field)) {
					FAIL("field mismatch at row " + to_string(row) + ": got [" + to_string(begin) + "," +
					     to_string(end) + "," + to_string(flag) + "] expected [" +
					     to_string(std::get<0>(expected_field)) + "," + to_string(std::get<1>(expected_field)) + "," +
					     to_string(std::get<2>(expected_field)) + "]: " + description);
				}
				begin = end + 1;
			}
		}
		if (field != fields.end_count) {
			FAIL("extra fields: " + description);
		}
	}
	REQUIRE(accepted > count / 10);
}
