#include "duckdb/formats/csv_splitter.hpp"

#include "duckdb/common/bit_utils.hpp"
#include "duckdb/formats/csv_simd.hpp"

namespace duckdb {

namespace {

bool StartsRow(CSVState state) {
	return state == CSVState::RECORD_SEPARATOR || state == CSVState::NOT_SET;
}

CSVState Terminated(const CsvBlockBits &bits, uint64_t bit) {
	return bits.carriage & bit ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR;
}

} // namespace

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

CsvSkip::CsvSkip(const CsvGrammar &grammar_p, idx_t rows_p)
    : grammar(grammar_p), rows(rows_p), indexer(grammar_p, true) {
}

bool CsvSkip::Feed(const char *data, idx_t base, idx_t end) {
	while (pos < end) {
		const idx_t block = pos;
		const idx_t size = MinValue<idx_t>(CSV_BLOCK_BYTES, end - pos);
		CsvBlockBits bits;
		indexer.Index(data + (block - base), size, bits);
		const uint64_t standard_before = (bits.standard << 1) | (previous_standard ? 1 : 0);
		uint64_t edges = 0;
		if (block == base) {
			edges |= 1;
		}
		if (block + size == end) {
			edges |= uint64_t(1) << (size - 1);
		}
		const uint64_t continued = bits.commented & ~bits.comment & edges;
		previous_standard = (bits.standard >> (size - 1)) & 1;
		uint64_t events = bits.term | bits.comment | continued;
		while (events) {
			const idx_t i = CountZeros<uint64_t>::Trailing(events);
			events &= events - 1;
			const idx_t position = block + i;
			const uint64_t bit = uint64_t(1) << i;
			if (bits.comment & bit) {
				if (!(standard_before & bit)) {
					starts_as_comment = true;
				}
				comment = true;
				continue;
			}
			if (continued & bit) {
				starts_as_comment = true;
				continue;
			}
			const CSVState previous = position == last_term ? last_state : CSVState::STANDARD;
			const CSVState next = Terminated(bits, bit);
			bool done = false;
			if (next == CSVState::RECORD_SEPARATOR) {
				if (StartsRow(previous)) {
					done = AddRow();
					lines++;
				} else if (previous != CSVState::CARRIAGE_RETURN) {
					done = EndLine(true);
					lines++;
				}
			} else {
				if (StartsRow(previous)) {
					done = AddRow();
				} else if (previous != CSVState::CARRIAGE_RETURN) {
					done = EndLine(!grammar.only_crlf);
				}
				lines++;
			}
			last_term = position + 1;
			last_state = next;
			if (done) {
				pos = position + 1;
				return true;
			}
		}
		pos = block + size;
	}
	return false;
}

idx_t CsvSkip::Start(const CSVReaderOptions &options) const {
	const bool carry_on = options.dialect_options.state_machine_options.new_line == NewLineIdentifier::CARRY_ON;
	if (carry_on && last_term == pos && last_state == CSVState::CARRIAGE_RETURN) {
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
    : grammar(grammar_p), columns(columns_p), capacity(capacity_p), indexer(grammar), fields(columns_p * capacity_p),
      rows(capacity_p) {
}

void CsvSplitter::Reset(idx_t position, idx_t lines, const CsvRangeStart &start) {
	pos = position;
	states[0] = start.states[0];
	states[1] = start.states[1];
	indexer.Reset(states[1]);
	last_term = position;
	field_count = 0;
	last_position = start.last_position == DConstants::INVALID_INDEX ? position : start.last_position;
	quoted = false;
	ever_quoted = false;
	ever_escaped = false;
	used_unstrictness = false;
	escaped = false;
	comment = states[1] == CSVState::COMMENT;
	position_before_comment = position;
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
	indexer.Reset(states[1]);
	last_term = pos;
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

void CsvSplitter::Terminate(idx_t position, bool carriage_return) {
	const CSVState previous = position == last_term ? states[1] : (comment ? CSVState::COMMENT : CSVState::STANDARD);
	states[0] = previous;
	states[1] = carriage_return ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR;
	scan_pos = position;
	if (!carriage_return) {
		if (StartsRow(previous)) {
			EmptyLine(position);
			lines_read++;
		} else if (previous != CSVState::CARRIAGE_RETURN) {
			if (comment) {
				UnsetComment(position);
			} else {
				AddRow(position);
			}
			lines_read++;
		}
	} else {
		if (StartsRow(previous)) {
			EmptyLine(position);
		} else if (previous != CSVState::CARRIAGE_RETURN) {
			if (comment) {
				UnsetComment(position);
			} else if (!grammar.only_crlf) {
				AddRow(position);
			}
		}
		lines_read++;
	}
	pos = position + 1;
	last_term = pos;
	SaveBoundary();
}

void CsvSplitter::Finish() {
	finished = true;
	scan_pos = end;
	if (last_term != end) {
		states[0] = indexer.PreviousState();
		states[1] = indexer.State();
	}
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
	while (pos < end && row_count < max_rows) {
		if (pos >= stop_at && pos == boundary.pos) {
			finished = true;
			return row_count;
		}
		const idx_t block = pos;
		const idx_t size = MinValue<idx_t>(CSV_BLOCK_BYTES, end - pos);
		CsvBlockBits bits;
		indexer.Index(bytes + (block - base), size, bits);
		uint64_t events =
		    bits.sep | bits.term | bits.open | bits.mark | bits.ever | bits.quoted_nl | bits.comment | bits.invalid;
		bool stopped = false;
		while (events) {
			const idx_t i = CountZeros<uint64_t>::Trailing(events);
			events &= events - 1;
			const uint64_t bit = uint64_t(1) << i;
			const idx_t position = block + i;
			if (bits.sep & bit) {
				AddValue(position);
				continue;
			}
			if (bits.term & bit) {
				Terminate(position, bits.carriage & bit);
				if (row_count >= max_rows || pos >= stop_at) {
					stopped = true;
					break;
				}
				continue;
			}
			if (bits.open & bit) {
				if (quoted && has_escape) {
					escaped = true;
					ever_escaped = true;
				}
				if (!quoted) {
					quoted_position = position;
				}
				quoted = true;
				ever_quoted = true;
				continue;
			}
			if (bits.invalid & bit) {
				if (quoted) {
					Event(CsvRowEventKind::UNTERMINATED_QUOTES, field_count, last_position);
				} else {
					Event(CsvRowEventKind::INVALID_STATE, field_count, last_position);
				}
				continue;
			}
			if (bits.quoted_nl & bit) {
				row_flags |= CSV_ROW_QUOTED_NEWLINE;
				continue;
			}
			if (bits.comment & bit) {
				if (!comment) {
					position_before_comment = position;
					comment = true;
				}
				continue;
			}
			if (bits.mark & bit) {
				escaped = true;
			}
			if (bits.unstrict & bit) {
				used_unstrictness = true;
			}
			if (bits.ever & bit) {
				ever_escaped = true;
			}
		}
		if (stopped) {
			indexer.Reset(states[1]);
			continue;
		}
		pos = block + size;
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

CsvRangeEntry CsvTransfer(const CsvGrammar &grammar, const char *bytes, idx_t base, idx_t begin, idx_t end,
                          CSVState origin) {
	CsvRangeEntry entry;
	entry.computed = true;
	CsvIndexer indexer(grammar);
	indexer.Reset(origin);
	CsvRangeStart current;
	current.states[0] = origin;
	current.states[1] = origin;
	idx_t last_term = begin;
	CSVState term_state = origin;
	bool comment = origin == CSVState::COMMENT;
	for (idx_t pos = begin; pos < end;) {
		const idx_t size = MinValue<idx_t>(CSV_BLOCK_BYTES, end - pos);
		CsvBlockBits bits;
		indexer.Index(bytes + (pos - base), size, bits);
		uint64_t events = bits.term | bits.comment;
		while (events) {
			const idx_t i = CountZeros<uint64_t>::Trailing(events);
			events &= events - 1;
			const uint64_t bit = uint64_t(1) << i;
			const idx_t position = pos + i;
			if (bits.comment & bit) {
				comment = true;
				continue;
			}
			const CSVState previous =
			    position == last_term ? term_state : (comment ? CSVState::COMMENT : CSVState::STANDARD);
			const CSVState next = Terminated(bits, bit);
			const bool empty = StartsRow(previous);
			const bool commented = previous == CSVState::COMMENT;
			const bool silent = previous == CSVState::CARRIAGE_RETURN ||
			                    (!empty && !commented && next == CSVState::CARRIAGE_RETURN && grammar.only_crlf);
			if (!silent) {
				if (!empty) {
					current.line_end = position;
				}
				const bool skip_line_feed =
				    next == CSVState::CARRIAGE_RETURN || (commented && grammar.carry_on_newline);
				current.last_position = position + 1 + skip_line_feed;
			}
			current.states[0] = previous;
			current.states[1] = next;
			comment = false;
			last_term = position + 1;
			term_state = next;
			if (entry.first_boundary == DConstants::INVALID_INDEX) {
				entry.first_boundary = position + 1;
				entry.boundary = current;
			}
		}
		pos += size;
	}
	entry.end = current;
	if (last_term != end) {
		entry.end.states[0] = indexer.PreviousState();
		entry.end.states[1] = indexer.State();
	}
	return entry;
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
