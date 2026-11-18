#include "duckdb/formats/csv_splitter.hpp"

#include "duckdb/common/bit_utils.hpp"

namespace duckdb {

CsvGrammar CsvGrammar::For(const CSVStateMachine &state_machine) {
	CsvGrammar grammar;
	auto &dialect = state_machine.dialect_options.state_machine_options;
	const auto delimiter = dialect.delimiter.GetValue();
	grammar.transitions = &state_machine.transition_array;
	grammar.extra_delimiter_bytes = delimiter.empty() ? 0 : delimiter.size() - 1;
	grammar.quote = dialect.quote.GetValue();
	grammar.escape = dialect.escape.GetValue();
	grammar.strict_mode = dialect.strict_mode.GetValue();
	grammar.carry_on_newline = dialect.new_line.GetValue() == NewLineIdentifier::CARRY_ON;
	auto &machine = state_machine.state_machine_options;
	grammar.only_crlf = machine.strict_mode.GetValue() && machine.strict_mode.IsSetByUser() &&
	                    machine.new_line.GetValue() == NewLineIdentifier::CARRY_ON && machine.new_line.IsSetByUser();
	grammar.trim_quoted_spaces = (delimiter.empty() || delimiter[0] != ' ') && dialect.quote.GetValue() != ' ' &&
	                             dialect.escape.GetValue() != ' ' && dialect.comment.GetValue() != ' ';
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

CsvSkip::CsvSkip(const CsvGrammar &grammar_p, idx_t rows_p) : grammar(grammar_p), rows(rows_p) {
}

bool CsvSkip::Feed(const char *data, idx_t base, idx_t end) {
	auto &table = *grammar.transitions;
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
				done = EndLine(!grammar.only_crlf);
			}
			lines++;
			break;
		case CSVState::COMMENT:
			if ((!comment || pos == base || pos + 1 == end) && states[0] != CSVState::STANDARD) {
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

idx_t CsvSkip::Start(const CSVReaderOptions &options) const {
	const bool carry_on = options.dialect_options.state_machine_options.new_line == NewLineIdentifier::CARRY_ON;
	if (carry_on && states[1] == CSVState::CARRIAGE_RETURN) {
		return pos + 1;
	}
	return pos;
}

bool CsvSkip::AddRow() {
	skipped++;
	return skipped >= rows;
}

bool CsvSkip::EndLine(bool add) {
	if (comment) {
		const bool counted = !starts_as_comment;
		comment = false;
		starts_as_comment = false;
		return counted && AddRow();
	}
	return add && AddRow();
}

CsvSplitter::CsvSplitter(const CsvGrammar &grammar_p, idx_t columns_p, idx_t capacity_p)
    : grammar(grammar_p), columns(columns_p), capacity(capacity_p), skipper(grammar_p.transitions->stop_patterns),
      fields(columns_p * capacity_p), rows(capacity_p) {
}

void CsvTransfer(const CsvGrammar &grammar, const char *bytes, idx_t base, idx_t begin, idx_t end,
                 CsvRangeTransfer &transfer) {
	struct Run {
		CsvRangeStart start;
		uint32_t origins;
	};
	constexpr idx_t STATES = StateMachine::NUM_STATES;
	constexpr idx_t NONE = DConstants::INVALID_INDEX;
	const auto &table = *grammar.transitions;
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
	auto self_loop = [](CSVState state) {
		return state == CSVState::STANDARD || state == CSVState::QUOTED || state == CSVState::COMMENT;
	};
	CSVByteSkipper skipper(table.stop_patterns);
	skipper.SetBuffer(bytes, end - base);
	for (idx_t pos = begin; pos < end; pos++) {
		bool skippable = run_count > 0;
		for (idx_t i = 0; i < run_count; i++) {
			skippable = skippable && self_loop(runs[i].start.states[1]);
		}
		if (skippable && pos > begin) {
			idx_t relative = pos - base;
			if (skipper.SkipToCandidate(end - base, relative) != CSVByteSkipper::SkippedTo::TAIL &&
			    relative != pos - base) {
				for (idx_t i = 0; i < run_count; i++) {
					runs[i].start.states[0] = runs[i].start.states[1];
				}
				pos = base + relative;
				if (pos >= end) {
					break;
				}
			}
		}
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
			const bool silent = previous == CSVState::CARRIAGE_RETURN ||
			                    (!empty && !comment && next == CSVState::CARRIAGE_RETURN && grammar.only_crlf);
			if (!silent) {
				if (!empty) {
					run.start.line_end = pos;
				}
				const bool skip_line_feed = next == CSVState::CARRIAGE_RETURN || (comment && grammar.carry_on_newline);
				run.start.last_position = pos + 1 + skip_line_feed;
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

void CsvSplitter::Reset(idx_t position, idx_t lines, const CsvRangeStart &start) {
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

void CsvSplitter::ClearRows() {
	row_count = 0;
	events.clear();
	extras.clear();
}

void CsvSplitter::SaveBoundary() {
	boundary.pos = pos;
	boundary.states[0] = states[0];
	boundary.states[1] = states[1];
	boundary.last_position = last_position;
	boundary.line_begin = line_begin;
	boundary.line_end = line_end;
	boundary.lines_read = lines_read;
	boundary.ever_quoted = ever_quoted;
}

void CsvSplitter::RestoreBoundary() {
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

void CsvSplitter::Event(CsvRowEventKind kind, idx_t column, idx_t position) {
	events.push_back({row_count, column, position, kind});
	row_flags |= CSV_ROW_EVENTS;
}

void CsvSplitter::AddField(idx_t offset, idx_t size, bool empty) {
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

void CsvSplitter::AddQuotedValue(idx_t position) {
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

void CsvSplitter::AddValue(idx_t position) {
	if (last_position > position) {
		return;
	}
	if (quoted) {
		AddQuotedValue(position - grammar.extra_delimiter_bytes);
	} else if (escaped) {
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

void CsvSplitter::AddRow(idx_t position) {
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
	rows[row_count] = {
	    boundary.pos, line_begin, line_end, last_position, lines_read, static_cast<uint32_t>(field_count), row_flags};
	row_count++;
	field_count = 0;
	row_flags = 0;
}

void CsvSplitter::EmptyLine(idx_t position) {
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

void CsvSplitter::UnsetComment(idx_t position) {
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
	line_begin = line_end;
	line_end = scan_pos;
	field_count = 0;
}

void CsvSplitter::Finish() {
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

idx_t CsvSplitter::Split(const char *bytes_p, idx_t base_p, idx_t end_p, idx_t max_rows, bool last, idx_t stop_at,
                         bool append) {
	bytes = bytes_p;
	base = base_p;
	end = end_p;
	if (!append) {
		ClearRows();
	}
	if (finished) {
		return row_count;
	}
	const bool has_escape = grammar.escape != '\0';
	const auto &table = *grammar.transitions;
	skipper.SetBuffer(bytes, end - base);
	while (pos < end && row_count < max_rows) {
		if (pos >= stop_at && pos == boundary.pos) {
			finished = true;
			return row_count;
		}
		scan_pos = pos;
		Transition(At(pos));
		switch (states[1]) {
		case CSVState::INVALID:
			if (quoted) {
				Event(CsvRowEventKind::UNTERMINATED_QUOTES, field_count, last_position);
			} else {
				Event(CsvRowEventKind::INVALID_STATE, field_count, last_position);
			}
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
				} else if (!grammar.only_crlf) {
					AddRow(pos);
				}
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
			Skip(table.skip_quoted);
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
		case CSVState::STANDARD:
			pos++;
			Skip(table.skip_standard);
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
			Skip(table.skip_comment);
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

} // namespace duckdb
