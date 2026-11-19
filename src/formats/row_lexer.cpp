#include "duckdb/formats/row_lexer.hpp"

#include "duckdb/formats/csv_simd.hpp"

#include <bit>
#include <cstring>

#if defined(__BMI2__)
#include <immintrin.h>
#endif

namespace duckdb {

namespace {

constexpr idx_t FIELD_SLACK = 8;

uint64_t ExpectedPattern(idx_t columns, idx_t phase) {
	uint64_t result = 0;
	for (idx_t i = 0; i < 64; i++) {
		if ((i + phase) % columns == columns - 1) {
			result |= uint64_t(1) << i;
		}
	}
	return result;
}

inline uint32_t *WritePositions(uint64_t bits, uint32_t offset, uint32_t *out) {
	const int count = std::popcount(bits);
	for (int i = 0; i < count; i += 4) {
		out[i] = offset + static_cast<uint32_t>(std::countr_zero(bits));
		bits &= bits - 1;
		out[i + 1] = offset + static_cast<uint32_t>(std::countr_zero(bits));
		bits &= bits - 1;
		out[i + 2] = offset + static_cast<uint32_t>(std::countr_zero(bits));
		bits &= bits - 1;
		out[i + 3] = offset + static_cast<uint32_t>(std::countr_zero(bits));
		bits &= bits - 1;
	}
	return out + count;
}

inline uint64_t FindEscaped(uint64_t escapes, uint64_t &carry) {
	if (!escapes) {
		const uint64_t escaped = carry;
		carry = 0;
		return escaped;
	}
	constexpr uint64_t ODD_BITS = 0xAAAAAAAAAAAAAAAAULL;
	const uint64_t potential = escapes & ~carry;
	const uint64_t codes = (((potential << 1) | ODD_BITS) - potential) ^ ODD_BITS;
	const uint64_t escaped = codes ^ (escapes | carry);
	carry = (codes & escapes) >> 63;
	return escaped;
}

void WalkQuotes(const char *block, idx_t size, char quote, char escape, uint64_t &in_quotes, uint64_t &pending,
                uint64_t &quotes, uint64_t &inside, uint64_t &escaped) {
	bool open = in_quotes != 0;
	bool escape_next = pending != 0;
	quotes = 0;
	inside = 0;
	escaped = 0;
	for (idx_t i = 0; i < size; i++) {
		const uint64_t bit = uint64_t(1) << i;
		const char ch = block[i];
		if (escape_next) {
			escaped |= bit;
			escape_next = false;
		} else if (ch == quote) {
			open = !open;
			quotes |= bit;
		} else if (open && ch == escape) {
			escape_next = true;
		}
		if (open) {
			inside |= bit;
		}
	}
	in_quotes = open;
	pending = escape_next;
}

uint64_t KeepFieldQuotes(uint64_t quote, uint64_t in_carry, uint64_t delimiter, idx_t delimiter_size,
                         uint64_t term_bits, uint64_t raw_spaces, uint64_t after_separator, uint64_t start_space,
                         uint64_t after_closer, bool reopen, uint64_t valid) {
	while (quote) {
		const uint64_t in = CsvPrefixXor(quote) ^ in_carry;
		const uint64_t terms = term_bits & ~in & valid;
		const uint64_t delims = delimiter & ~in & valid;
		const uint64_t field_starts = (delims << delimiter_size) | (terms << 1) | after_separator;
		const uint64_t starts = field_starts | ((field_starts & raw_spaces & ~in) << 1) | start_space;
		const uint64_t openers = quote & in;
		const uint64_t allowed = reopen ? (((quote & ~in) << 1) | after_closer) : 0;
		const uint64_t misplaced = openers & ~starts & ~allowed;
		if (!misplaced) {
			break;
		}
		quote &= ~(misplaced & (0 - misplaced));
	}
	return quote;
}

bool PlainCountState(CSVState state) {
	return state == CSVState::NOT_SET || state == CSVState::RECORD_SEPARATOR || state == CSVState::DELIMITER ||
	       state == CSVState::STANDARD || state == CSVState::EMPTY_SPACE;
}

bool PlainCountTransitions(const StateMachine &table, const RowDialect &d, char (&specials)[4], idx_t &count,
                           bool &line_feeds, bool &carriage_pairs) {
	count = 0;
	line_feeds = true;
	carriage_pairs = true;
	if (d.delimiter_size != 1) {
		return false;
	}
	bool special[StateMachine::NUM_TRANSITIONS] = {};
	special[static_cast<uint8_t>('\r')] = true;
	for (const char byte : {d.quote, d.escape, d.comment}) {
		if (byte != '\0' && !special[static_cast<uint8_t>(byte)]) {
			special[static_cast<uint8_t>(byte)] = true;
			specials[count++] = byte;
		}
	}
	const auto delimiter = static_cast<uint8_t>(d.delimiter);
	if (special[delimiter] || delimiter == '\n' || special[static_cast<uint8_t>('\n')]) {
		return false;
	}
	const CSVState states[] = {CSVState::NOT_SET, CSVState::RECORD_SEPARATOR, CSVState::DELIMITER, CSVState::STANDARD,
	                           CSVState::EMPTY_SPACE};
	carriage_pairs = table[static_cast<uint8_t>('\n')][static_cast<uint8_t>(CSVState::CARRIAGE_RETURN)] ==
	                 CSVState::RECORD_SEPARATOR;
	for (const auto state : states) {
		const auto from = static_cast<uint8_t>(state);
		if (table[delimiter][from] != CSVState::DELIMITER) {
			return false;
		}
		line_feeds = line_feeds && table[static_cast<uint8_t>('\n')][from] == CSVState::RECORD_SEPARATOR;
		carriage_pairs = carriage_pairs && table[static_cast<uint8_t>('\r')][from] == CSVState::CARRIAGE_RETURN;
		for (idx_t byte = 0; byte < StateMachine::NUM_TRANSITIONS; byte++) {
			if (byte == delimiter || byte == '\n' || byte == ' ' || special[byte]) {
				continue;
			}
			if (table[byte][from] != CSVState::STANDARD) {
				return false;
			}
		}
		if (delimiter != ' ') {
			const auto space = table[static_cast<uint8_t>(' ')][from];
			if (space != CSVState::STANDARD && space != CSVState::EMPTY_SPACE) {
				return false;
			}
			if ((state == CSVState::STANDARD || state == CSVState::EMPTY_SPACE) && space != CSVState::STANDARD) {
				return false;
			}
		}
	}
	return true;
}

idx_t RunStops(const StateMachine &table, CSVState first, CSVState second, char (&stops)[8]) {
	idx_t count = 0;
	for (idx_t byte = 0; byte < StateMachine::NUM_TRANSITIONS; byte++) {
		const auto from_first = table[byte][static_cast<uint8_t>(first)];
		const auto from_second = table[byte][static_cast<uint8_t>(second)];
		const bool stays_first = from_first == first || from_first == second;
		const bool stays_second = from_second == first || from_second == second;
		if (stays_first != stays_second || (!stays_first && from_first != from_second)) {
			return DConstants::INVALID_INDEX;
		}
		if (stays_first) {
			continue;
		}
		if (count == 8) {
			return DConstants::INVALID_INDEX;
		}
		stops[count++] = static_cast<char>(byte);
	}
	return count;
}

} // namespace

RowDialect RowDialect::DuckDB(const CsvGrammar &grammar) {
	RowDialect result;
	result.delimiter = grammar.delimiter[0];
	result.delimiter_size = grammar.delimiter_size;
	bool plain_delimiter = true;
	for (idx_t i = 0; i < grammar.delimiter_size; i++) {
		const char byte = grammar.delimiter[i];
		plain_delimiter = plain_delimiter && byte != '\n' && byte != '\r' && (i == 0 || byte != '\0');
		if (i > 0) {
			result.delimiter_tail[i - 1] = byte;
		}
	}
	result.distinct_delimiter = grammar.distinct_delimiter;
	result.quote = grammar.quote;
	result.escape = grammar.escape;
	result.comment = grammar.comment;
	result.quoting = grammar.quote ? RowQuoting::FIELD_START : RowQuoting::NONE;
	result.escape_in_quotes = grammar.quoted_escape && grammar.strict_mode;
	result.reject_escape = grammar.quoted_escape && !grammar.strict_mode;
	result.carriage_terms = true;
	result.merge_terms = true;
	result.carry_on = grammar.carry_on_newline;
	result.strict = grammar.strict_mode;
	result.reopen = grammar.quote == grammar.escape || (!grammar.strict_mode && grammar.escape != '\0');
	result.quoted_spaces = grammar.quote != '\0' && grammar.trim_quoted_spaces;
	result.new_line = grammar.new_line;
	result.supported = plain_delimiter;
	result.transitions = grammar.transitions;
	return result;
}

RowDialect RowDialect::PostgresCsv(char delimiter, char quote, char escape) {
	RowDialect result;
	result.delimiter = delimiter;
	result.quote = quote;
	result.escape = escape;
	result.quoting = RowQuoting::ANYWHERE;
	result.escape_in_quotes = escape != '\0' && escape != quote;
	result.strip_carriage = true;
	return result;
}

RowDialect RowDialect::PostgresText(char delimiter) {
	RowDialect result;
	result.delimiter = delimiter;
	result.escape = '\\';
	result.backslash = true;
	result.strip_carriage = true;
	return result;
}

void RowFields::Reserve(idx_t more) {
	const idx_t needed = MaxValue(row_count, end_count) + more + FIELD_SLACK;
	if (needed <= capacity) {
		return;
	}
	const idx_t grown = MaxValue(needed, capacity * 2);
	auto new_rows = make_unsafe_uniq_array_uninitialized<uint32_t>(grown);
	auto new_ends = make_unsafe_uniq_array_uninitialized<uint32_t>(grown);
	if (row_count) {
		memcpy(new_rows.get(), rows.get(), row_count * sizeof(uint32_t));
	}
	if (end_count) {
		memcpy(new_ends.get(), ends.get(), end_count * sizeof(uint32_t));
	}
	rows = std::move(new_rows);
	ends = std::move(new_ends);
	capacity = grown;
}

void RowFields::Drop(idx_t count, idx_t dropped_ends) {
	D_ASSERT(count <= row_count && dropped_ends <= end_count);
	memmove(rows.get(), rows.get() + count, (row_count - count) * sizeof(uint32_t));
	memmove(ends.get(), ends.get() + dropped_ends, (end_count - dropped_ends) * sizeof(uint32_t));
	row_count -= count;
	end_count -= dropped_ends;
}

RowLexer::RowLexer(const RowDialect &dialect_p, idx_t columns_p, idx_t max_line_size_p, bool error_mode,
                   bool pad_rows_p)
    : dialect(dialect_p), columns(columns_p), max_line_size(max_line_size_p), pad_rows(pad_rows_p) {
	eligible = dialect.supported && columns >= 1 && max_line_size >= CSV_BLOCK_BYTES;
	settle_strict = error_mode && dialect.strict;
	settle_length = error_mode;
	if (eligible) {
		patterns.resize(columns);
		for (idx_t phase = 0; phase < columns; phase++) {
			patterns[phase] = ExpectedPattern(columns, phase);
		}
	}
	if (dialect.transitions) {
		const auto &table = *dialect.transitions;
		count_plain = PlainCountTransitions(table, dialect, count_specials, count_special_count, count_line_feeds,
		                                    count_carriage_pairs);
		standard_stop_count = RunStops(table, CSVState::STANDARD, CSVState::STANDARD, standard_stops);
		quoted_stop_count = RunStops(table, CSVState::QUOTED, CSVState::QUOTED_NEW_LINE, quoted_stops);
		comment_stop_count = RunStops(table, CSVState::COMMENT, CSVState::COMMENT, comment_stops);
	}
}

void RowLexer::Start(RowCursor &cursor, idx_t begin, bool settled, bool check_arity) const {
	cursor = RowCursor();
	cursor.origin = begin;
	cursor.pos = begin;
	cursor.settled = settled || dialect.quoting != RowQuoting::FIELD_START;
	cursor.check_arity = check_arity;
	cursor.ok = eligible;
}

bool RowLexer::Feed(RowCursor &cursor, const char *data, idx_t base, idx_t end, bool last, RowFields *fields) const {
	if (fields) {
		return FeedBlocks<true>(cursor, data, base, end, last, fields);
	}
	return FeedBlocks<false>(cursor, data, base, end, last, nullptr);
}

template <bool EXTRACT>
bool RowLexer::FeedBlocks(RowCursor &cursor, const char *data, idx_t base, idx_t end, bool last,
                          RowFields *fields) const {
	const auto &d = dialect;
	if (!cursor.ok) {
		return false;
	}
	const bool field_start_quotes = d.quoting == RowQuoting::FIELD_START;
	const bool escapes = d.backslash || d.escape_in_quotes;
	const uint64_t quote_follow_bit = d.reopen ? ~uint64_t(0) : 0;
	auto c = cursor;
	uint32_t *rows_out = nullptr;
	uint32_t *ends_out = nullptr;
	if (EXTRACT) {
		fields->Reserve(end - c.pos + CSV_BLOCK_BYTES + 1);
		rows_out = fields->rows.get() + fields->row_count;
		ends_out = fields->ends.get() + fields->end_count;
	}
	auto fail = [&]() {
		c.ok = false;
		if (EXTRACT) {
			fields->row_count = static_cast<idx_t>(rows_out - fields->rows.get());
			fields->end_count = static_cast<idx_t>(ends_out - fields->ends.get());
		}
		cursor = c;
		return false;
	};
	const idx_t delimiter_size = d.delimiter_size;
	const bool literal_quotes =
	    field_start_quotes && !escapes && !d.comment && (delimiter_size == 1 || d.distinct_delimiter);
	uint64_t literals = 0;
	idx_t literals_at = DConstants::INVALID_INDEX;
	char padded[CSV_BLOCK_BYTES];
	char lookahead[CSV_BLOCK_BYTES];
	while (c.pos < end) {
		const idx_t size = MinValue<idx_t>(CSV_BLOCK_BYTES, end - c.pos);
		if (size < CSV_BLOCK_BYTES && !last) {
			break;
		}
		if (delimiter_size > 1 && !last && c.pos + CSV_BLOCK_BYTES + delimiter_size - 1 > end) {
			break;
		}
		const char *block = data + (c.pos - base);
		uint64_t valid = ~uint64_t(0);
		if (size < CSV_BLOCK_BYTES) {
			memset(padded, 0, sizeof(padded));
			memcpy(padded, block, size);
			block = padded;
			valid = (uint64_t(1) << size) - 1;
		}
		const CsvVector vector(block);
		c.non_ascii |= vector.NonAscii();
		if (d.reject_escape && (vector.Equal(d.escape) & valid)) {
			return fail();
		}
		uint64_t delimiter = vector.Equal(d.delimiter);
		if (delimiter_size > 1) {
			memset(lookahead, 0, sizeof(lookahead));
			if (size == CSV_BLOCK_BYTES) {
				memcpy(lookahead, block + CSV_BLOCK_BYTES,
				       MinValue<idx_t>(delimiter_size - 1, end - (c.pos + CSV_BLOCK_BYTES)));
			}
			const CsvVector next(lookahead);
			for (idx_t i = 1; i < delimiter_size; i++) {
				const char byte = d.delimiter_tail[i - 1];
				delimiter &= (vector.Equal(byte) >> i) | (next.Equal(byte) << (CSV_BLOCK_BYTES - i));
			}
			delimiter &= valid;
		}
		const uint64_t line_feed = vector.Equal('\n');
		const uint64_t carriage_return = vector.Equal('\r');
		uint64_t escape_bits = 0;
		uint64_t escaped = 0;
		const uint64_t carried_escape = c.escape_carry;
		if (escapes) {
			escape_bits = vector.Equal(d.escape) & valid;
			escaped = FindEscaped(escape_bits, c.escape_carry);
			if (size < CSV_BLOCK_BYTES) {
				c.escape_carry = ((escape_bits & ~escaped) >> (size - 1)) & 1;
			}
		}
		const uint64_t raw_quote = d.quote ? vector.Equal(d.quote) & valid : 0;
		uint64_t quote = d.escape_in_quotes ? raw_quote & ~escaped : raw_quote;
		if (literals_at == c.pos) {
			quote &= ~literals;
		}
		uint64_t in = (quote ? CsvPrefixXor(quote) : 0) ^ CsvSpread(c.in_quotes);
		if (d.escape_in_quotes) {
			if (raw_quote & escaped & ~in) {
				uint64_t in_quotes = c.in_quotes;
				uint64_t pending = carried_escape;
				WalkQuotes(block, size, d.quote, d.escape, in_quotes, pending, quote, in, escaped);
				in &= valid;
				c.escape_carry = pending;
			} else {
				c.escape_carry &= (in >> (size - 1)) & 1;
			}
			if (field_start_quotes) {
				if (escaped & in & ~(raw_quote | escape_bits)) {
					return fail();
				}
				c.doubled = c.doubled || (escape_bits & in) != 0;
			}
		}
		const uint64_t hidden = d.backslash ? (in | escaped) : in;
		const uint64_t term_bits = d.carriage_terms ? (line_feed | carriage_return) : line_feed;
		const uint64_t terms = term_bits & ~hidden & valid;
		uint64_t delims = delimiter & ~hidden & valid;
		if (delimiter_size > 1 && !d.distinct_delimiter) {
			uint64_t covered = c.delimiter_tail;
			uint64_t next_covered = 0;
			for (idx_t i = 1; i < delimiter_size; i++) {
				covered |= delims << i;
				next_covered |= delims >> (CSV_BLOCK_BYTES - i);
			}
			if (delims & covered) {
				return fail();
			}
			c.delimiter_tail = next_covered;
		}
		uint64_t comment_ends = 0;
		uint64_t region = 0;
		if (d.comment) {
			const uint64_t comments = vector.Equal(d.comment) & ~hidden & valid;
			if (comments | c.in_comment) {
				const uint64_t line_starts = ((terms << 1) | c.after_term) & valid;
				if (c.in_comment) {
					const uint64_t stop = terms & (0 - terms);
					region = stop ? stop - 1 : valid;
					c.in_comment = stop ? 0 : 1;
				}
				for (uint64_t pending = comments & ~region & line_starts; pending; pending &= ~region) {
					const uint64_t start = pending & (0 - pending);
					const uint64_t after = terms & ~((start << 1) - 1);
					const uint64_t stop = after & (0 - after);
					region |= stop ? stop - start : valid & ~(start - 1);
					if (!stop) {
						c.in_comment = 1;
					}
				}
				if ((comments & ~region) || (raw_quote & region)) {
					return fail();
				}
				delims &= ~region;
				comment_ends = terms & ((region << 1) | c.comment_tail);
				if (d.carry_on && (comment_ends & line_feed)) {
					return fail();
				}
			}
			c.comment_tail = (region >> (size - 1)) & 1;
		}
		const uint64_t carriage = carriage_return & terms;
		const uint64_t feeds = line_feed & terms;
		if (d.carry_on) {
			const uint64_t last_bit = uint64_t(1) << (size - 1);
			if ((carriage & ~(line_feed >> 1) & ~last_bit) || (c.after_cr && !(line_feed & 1))) {
				return fail();
			}
			if (d.strict && (feeds & ~((carriage_return << 1) | c.after_cr))) {
				return fail();
			}
		} else if (d.strict && d.carriage_terms) {
			if ((d.new_line == NewLineIdentifier::SINGLE_N && carriage) ||
			    (d.new_line == NewLineIdentifier::SINGLE_R && feeds)) {
				return fail();
			}
		}
		const uint64_t field_starts = (delims << delimiter_size) | (terms << 1) | c.after_separator;
		const uint64_t spaces = d.quoted_spaces ? vector.Equal(' ') & ~hidden & valid : 0;
		if (field_start_quotes && (quote | c.after_closer | c.after_opener | c.closer_spaces)) {
			const uint64_t openers = quote & in;
			const uint64_t closers = quote & ~in;
			const uint64_t starts = field_starts | ((field_starts & spaces) << 1) | c.start_space;
			const uint64_t reopened = (closers << 1) | c.after_closer;
			const uint64_t allowed = d.reopen ? reopened : 0;
			if (openers & ~starts & ~allowed) {
				if (!literal_quotes || !c.settled || literals_at == c.pos) {
					return fail();
				}
				const uint64_t raw_spaces = d.quoted_spaces ? vector.Equal(' ') & valid : 0;
				literals = quote & ~KeepFieldQuotes(quote, CsvSpread(c.in_quotes), delimiter, delimiter_size, term_bits,
				                                    raw_spaces, c.after_separator, c.start_space, c.after_closer,
				                                    d.reopen, valid);
				literals_at = c.pos;
				continue;
			}
			const uint64_t ends_field = delimiter | line_feed | carriage_return;
			const uint64_t follow = ends_field | (quote & quote_follow_bit);
			const uint64_t trailing = (reopened & spaces) | (c.closer_spaces & spaces & 1);
			const uint64_t after_trailing = spaces + trailing;
			const uint64_t trailed = (after_trailing & ~spaces) | (c.closer_spaces & ~spaces & 1);
			if (((reopened & ~follow & ~spaces) | (trailed & ~ends_field)) & valid) {
				return fail();
			}
			c.closer_spaces = trailing && after_trailing < spaces;
			c.doubled = c.doubled || (reopened & quote) != 0;
			if (!c.settled && c.quoted_newline == DConstants::INVALID_INDEX) {
				const uint64_t quoted_newlines = (line_feed | carriage_return) & in & valid;
				const idx_t limit =
				    quoted_newlines ? c.pos + std::countr_zero(quoted_newlines) : NumericLimits<idx_t>::Maximum();
				const uint64_t broken = ((openers << 1) | c.after_opener) & ~follow & ~spaces & valid;
				if (settle_strict && broken && c.pos + std::countr_zero(broken) < limit) {
					c.settled = true;
				} else if (quoted_newlines) {
					if (settle_length && limit - c.origin >= max_line_size) {
						c.settled = true;
					} else {
						c.quoted_newline = limit;
					}
				}
			}
			c.after_closer = (closers >> (size - 1)) & 1;
			c.after_opener = (openers >> (size - 1)) & 1;
		}
		c.start_space = ((field_starts & spaces) >> (size - 1)) & 1;
		c.quotes = c.quotes || raw_quote != 0;
		if (!c.settled && settle_length && c.quoted_newline == DConstants::INVALID_INDEX &&
		    c.pos + size - c.origin > max_line_size) {
			c.settled = true;
		}
		const uint64_t after_terms = (terms << 1) | c.after_term;
		uint64_t merged = 0;
		if (d.merge_terms) {
			merged = !d.empty_lines ? after_terms : d.carry_on ? (carriage << 1) | c.after_cr : 0;
		}
		const uint64_t row_ends = terms & ~merged & ~comment_ends;
		const uint64_t line_ends = (d.empty_lines ? row_ends & ~after_terms : row_ends) | comment_ends;
		const uint64_t boundaries = delims | row_ends;
		idx_t pad_count = 0;
		idx_t pad_total = 0;
		uint8_t pad_at[CSV_BLOCK_BYTES];
		uint32_t pad_size[CSV_BLOCK_BYTES];
		if (c.check_arity && boundaries) {
			const idx_t count = std::popcount(boundaries);
#if defined(__BMI2__)
			const uint64_t pattern = _pext_u64(row_ends, boundaries);
#else
			uint64_t pattern = 0;
			for (uint64_t rows = row_ends; rows; rows &= rows - 1) {
				pattern |= uint64_t(1) << std::popcount(boundaries & ((rows & (0 - rows)) - 1));
			}
#endif
			const uint64_t mask = count == 64 ? ~uint64_t(0) : (uint64_t(1) << count) - 1;
			if (!((pattern ^ patterns[c.phase]) & mask)) {
				c.phase = pattern ? count + std::countl_zero(pattern) - 64 : c.phase + count;
			} else if (pad_rows) {
				idx_t index = 0;
				for (uint64_t rest = pattern; rest; rest &= rest - 1) {
					const idx_t at = std::countr_zero(rest);
					c.phase += at - index;
					if (c.phase >= columns) {
						return fail();
					}
					if (c.phase + 1 < columns) {
						pad_at[pad_count] = static_cast<uint8_t>(at);
						pad_size[pad_count] = static_cast<uint32_t>(columns - 1 - c.phase);
						pad_total += pad_size[pad_count++];
					}
					c.phase = 0;
					index = at + 1;
				}
				c.phase += count - index;
				c.padded = c.padded || pad_count;
			} else {
				return fail();
			}
			if (c.phase >= columns) {
				return fail();
			}
		}
		const idx_t first_limit = max_line_size / 2;
		if (line_ends) {
			const idx_t first = c.pos + std::countr_zero(line_ends);
			if (c.last_row_end == DConstants::INVALID_INDEX) {
				if (first - c.origin > first_limit) {
					return fail();
				}
				c.first_row_end = first;
			} else if (first - c.last_row_end > max_line_size) {
				return fail();
			}
			c.last_row_end = c.pos + 63 - std::countl_zero(line_ends);
		} else if (c.last_row_end == DConstants::INVALID_INDEX) {
			if (c.pos + size - c.origin > first_limit) {
				return fail();
			}
		} else if (c.pos + size - c.last_row_end > max_line_size) {
			return fail();
		}
		c.rows += std::popcount(row_ends);
		if (terms) {
			c.last_term = c.pos + 63 - std::countl_zero(terms);
			if (c.first_term == DConstants::INVALID_INDEX) {
				c.first_term = c.pos + std::countr_zero(terms);
			}
		}
		c.lines += std::popcount(terms);
		if (d.carry_on) {
			c.lines -= std::popcount(feeds & ((carriage << 1) | c.after_cr));
		}
		if (EXTRACT) {
			const uint32_t offset = static_cast<uint32_t>(c.pos - c.origin);
			const uint64_t follows = (terms << 1) | c.after_term;
			const uint64_t row_starts = follows & ~(terms & merged) & ~region & valid;
			rows_out = WritePositions(row_starts, offset, rows_out);
			uint32_t *block_ends = ends_out;
			if (d.strip_carriage) {
				const uint64_t plain_carriage = carriage_return & ~hidden & valid;
				const uint64_t stripped = row_ends & ((plain_carriage << 1) | c.after_plain_cr);
				c.after_plain_cr = (plain_carriage >> (size - 1)) & 1;
				if (stripped & 1) {
					*ends_out++ = offset - 1;
				}
				ends_out =
				    WritePositions((boundaries & ~stripped) | ((stripped & ~uint64_t(1)) >> 1), offset, ends_out);
			} else {
				ends_out = WritePositions(boundaries, offset, ends_out);
			}
			const uint64_t specials = raw_quote | escape_bits;
			if (specials | c.pending_flag) {
				uint64_t flagged = 0;
				if (c.pending_flag && boundaries) {
					flagged |= boundaries & (0 - boundaries);
				}
				for (uint64_t rest = specials; rest; rest &= rest - 1) {
					const uint64_t above = boundaries & (0 - (rest & (0 - rest)));
					if (!above) {
						break;
					}
					flagged |= above & (0 - above);
				}
				if (boundaries) {
					const uint64_t top = uint64_t(1) << (63 - std::countl_zero(boundaries));
					c.pending_flag = (specials & ~(top | (top - 1))) != 0;
				} else {
					c.pending_flag = c.pending_flag || specials != 0;
				}
				for (; flagged; flagged &= flagged - 1) {
					const idx_t index = std::popcount(boundaries & ((flagged & (0 - flagged)) - 1));
					block_ends[index] |= ROW_FIELD_FLAG;
				}
			}
			if (pad_count) {
				const idx_t first_end = static_cast<idx_t>(block_ends - fields->ends.get());
				const idx_t written = static_cast<idx_t>(ends_out - block_ends);
				const idx_t rows_used = static_cast<idx_t>(rows_out - fields->rows.get());
				const idx_t more = pad_total + (end - c.pos) + CSV_BLOCK_BYTES;
				if (MaxValue(rows_used, first_end + written) + more + FIELD_SLACK > fields->capacity) {
					fields->row_count = rows_used;
					fields->end_count = first_end + written;
					fields->Reserve(more);
					rows_out = fields->rows.get() + rows_used;
				}
				uint32_t *const block_start = fields->ends.get() + first_end;
				idx_t shift = pad_total;
				idx_t tail = written;
				for (idx_t k = pad_count; k-- > 0;) {
					const idx_t at = pad_at[k];
					memmove(block_start + at + 1 + shift, block_start + at + 1, (tail - at - 1) * sizeof(uint32_t));
					shift -= pad_size[k];
					std::fill_n(block_start + at + 1 + shift, pad_size[k], block_start[at] | ROW_FIELD_FLAG);
					tail = at + 1;
				}
				ends_out = block_start + written + pad_total;
			}
		}
		c.in_quotes = (in >> (size - 1)) & 1;
		c.after_separator = (delims >> (CSV_BLOCK_BYTES - delimiter_size)) | ((terms >> (size - 1)) & 1);
		c.after_term = (terms >> (size - 1)) & 1;
		c.after_cr = (carriage >> (size - 1)) & 1;
		c.pos += size;
	}
	if (EXTRACT) {
		fields->row_count = static_cast<idx_t>(rows_out - fields->rows.get());
		fields->end_count = static_cast<idx_t>(ends_out - fields->ends.get());
	}
	cursor = c;
	return true;
}

bool RowLexer::Finish(RowCursor &cursor, const char *data, idx_t base, idx_t end, bool eof, RowFields *fields,
                      CSVState origin_state, CsvRangeEntry &entry) const {
	auto &c = cursor;
	if (!c.ok || c.pos != end || c.in_quotes || c.escape_carry) {
		c.ok = false;
		return false;
	}
	const bool ends_with_term = end > c.origin && c.last_term == end - 1;
	if (!ends_with_term) {
		if (end != c.origin) {
			const idx_t limit = c.last_row_end == DConstants::INVALID_INDEX ? max_line_size / 2 : max_line_size;
			const idx_t prior = c.last_row_end == DConstants::INVALID_INDEX ? c.origin : c.last_row_end;
			const bool row = !c.in_comment;
			if (!eof || (row && c.check_arity && !pad_rows && c.phase != columns - 1) || end - prior > limit) {
				c.ok = false;
				return false;
			}
			c.lines++;
			c.unterminated = true;
			c.rows += row;
			const idx_t pads = row && c.check_arity && pad_rows ? columns - 1 - c.phase : 0;
			c.padded = c.padded || pads;
			if (row && fields) {
				fields->Reserve(1 + pads);
				auto position = static_cast<uint32_t>(end - c.origin);
				if (dialect.strip_carriage && c.after_plain_cr) {
					position--;
				}
				if (c.pending_flag) {
					position |= ROW_FIELD_FLAG;
				}
				fields->ends[fields->end_count++] = position;
				for (idx_t i = 0; i < pads; i++) {
					fields->ends[fields->end_count++] = position | ROW_FIELD_FLAG;
				}
			}
		}
	} else if (c.check_arity && c.phase != 0) {
		c.ok = false;
		return false;
	}
	entry = CsvRangeEntry();
	entry.computed = true;
	entry.end.states[0] = origin_state;
	entry.end.states[1] = origin_state;
	if (c.last_term != DConstants::INVALID_INDEX) {
		const bool carriage_end = dialect.carry_on && data[c.last_term - base] == '\r';
		entry.end.last_position = c.last_term + 1 + carriage_end;
		entry.end.states[1] = carriage_end ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR;
	}
	entry.end.line_end = c.last_row_end;
	if (c.first_term != DConstants::INVALID_INDEX) {
		entry.first_boundary = c.first_term + 1;
		entry.boundary.states[1] = CSVState::RECORD_SEPARATOR;
		entry.boundary.last_position = entry.first_boundary;
	}
	return true;
}

idx_t RowLexer::CountLines(const char *data, idx_t base, idx_t begin, idx_t end) const {
	const auto &d = dialect;
	const bool escapes = d.backslash || d.escape_in_quotes;
	idx_t lines = 0;
	uint64_t in_quotes = 0;
	uint64_t after_cr = 0;
	uint64_t escape_carry = 0;
	char padded[CSV_BLOCK_BYTES];
	for (idx_t pos = begin; pos < end; pos += CSV_BLOCK_BYTES) {
		const idx_t size = MinValue<idx_t>(CSV_BLOCK_BYTES, end - pos);
		const char *block = data + (pos - base);
		uint64_t valid = ~uint64_t(0);
		if (size < CSV_BLOCK_BYTES) {
			memset(padded, 0, sizeof(padded));
			memcpy(padded, block, size);
			block = padded;
			valid = (uint64_t(1) << size) - 1;
		}
		const CsvVector vector(block);
		uint64_t escaped = 0;
		if (escapes) {
			escaped = FindEscaped(vector.Equal(d.escape) & valid, escape_carry);
		}
		uint64_t quote = d.quote ? vector.Equal(d.quote) & valid : 0;
		if (d.escape_in_quotes) {
			quote &= ~escaped;
		}
		const uint64_t in = (quote ? CsvPrefixXor(quote) : 0) ^ CsvSpread(in_quotes);
		const uint64_t hidden = d.backslash ? (in | escaped) : in;
		const uint64_t line_feed = vector.Equal('\n');
		const uint64_t carriage_return = vector.Equal('\r');
		const uint64_t term_bits = d.carriage_terms ? (line_feed | carriage_return) : line_feed;
		const uint64_t terms = term_bits & ~hidden & valid;
		const uint64_t carriage = carriage_return & terms;
		lines += std::popcount(terms);
		if (d.carry_on) {
			lines -= std::popcount(line_feed & terms & ((carriage << 1) | after_cr));
		}
		in_quotes = (in >> (size - 1)) & 1;
		after_cr = (carriage >> (size - 1)) & 1;
	}
	return lines;
}

void RowLexer::CountRow(RowCountCursor &cursor, idx_t position, uint8_t flags, vector<RowCount> &rows) const {
	const CSVState before = cursor.states[0];
	const CSVState current = cursor.states[1];
	if ((before == CSVState::DELIMITER && (current == CSVState::RECORD_SEPARATOR ||
	                                       current == CSVState::CARRIAGE_RETURN || current == CSVState::DELIMITER)) ||
	    (before == CSVState::STANDARD && current == CSVState::DELIMITER)) {
		flags |= ROW_COUNT_EMPTY_LAST;
	}
	const idx_t end = position == DConstants::INVALID_INDEX ? position : position + 1;
	rows.push_back({cursor.delimiters + 1, cursor.empty_lines, cursor.line_start, end, flags});
	cursor.delimiters = 0;
	if (position != DConstants::INVALID_INDEX) {
		cursor.line_start = end;
	}
}

bool RowLexer::CountByte(RowCountCursor &cursor, char byte, idx_t position, vector<RowCount> &rows, idx_t limit) const {
	auto &c = cursor;
	const CSVState previous = c.states[1];
	const CSVState next = (*dialect.transitions)[static_cast<uint8_t>(byte)][static_cast<uint8_t>(previous)];
	c.states[0] = previous;
	c.states[1] = next;
	switch (next) {
	case CSVState::INVALID:
		c.invalid = true;
		return true;
	case CSVState::RECORD_SEPARATOR:
	case CSVState::CARRIAGE_RETURN:
		if (previous == CSVState::RECORD_SEPARATOR || previous == CSVState::NOT_SET) {
			c.empty_lines++;
			c.line_start = position + 1;
			return false;
		}
		if (previous == CSVState::CARRIAGE_RETURN) {
			return false;
		}
		if (c.comment) {
			CountRow(c, position, c.starts_as_comment ? ROW_COUNT_COMMENT : ROW_COUNT_MID_COMMENT, rows);
			c.comment = false;
			c.starts_as_comment = false;
		} else {
			CountRow(c, position, 0, rows);
		}
		return rows.size() >= limit;
	case CSVState::DELIMITER:
		c.delimiters++;
		return false;
	case CSVState::QUOTED:
		if ((previous == CSVState::UNQUOTED || previous == CSVState::MAYBE_QUOTED) && dialect.escape != '\0') {
			c.ever_escaped = true;
		}
		if ((previous == CSVState::ESCAPE || previous == CSVState::ESCAPED_RETURN ||
		     previous == CSVState::UNQUOTED_ESCAPE) &&
		    (byte == dialect.quote || !dialect.strict)) {
			c.ever_escaped = true;
			c.used_unstrictness = c.used_unstrictness || previous == CSVState::UNQUOTED_ESCAPE;
		}
		c.ever_quoted = true;
		return false;
	case CSVState::UNQUOTED:
		c.ever_escaped = c.ever_escaped || previous == CSVState::MAYBE_QUOTED;
		return false;
	case CSVState::UNQUOTED_ESCAPE:
		c.used_unstrictness = true;
		return false;
	case CSVState::COMMENT:
		if (previous != CSVState::COMMENT) {
			c.starts_as_comment = previous != CSVState::STANDARD && previous != CSVState::DELIMITER;
			c.comment = true;
		}
		return false;
	default:
		return false;
	}
}

CSVState RowLexer::PlainState(const char *block, idx_t position, idx_t first, CSVState incoming) const {
	const char byte = block[position];
	if (byte == dialect.delimiter) {
		return CSVState::DELIMITER;
	}
	if (byte == '\n') {
		return CSVState::RECORD_SEPARATOR;
	}
	if (byte == '\r') {
		return CSVState::CARRIAGE_RETURN;
	}
	if (byte != ' ') {
		return CSVState::STANDARD;
	}
	CSVState before = incoming;
	if (position > first) {
		const char previous = block[position - 1];
		before = previous == dialect.delimiter ? CSVState::DELIMITER
		         : previous == '\n'            ? CSVState::RECORD_SEPARATOR
		                                       : CSVState::STANDARD;
	}
	return (*dialect.transitions)[static_cast<uint8_t>(' ')][static_cast<uint8_t>(before)];
}

bool RowLexer::CountPlain(RowCountCursor &cursor, const char *block, idx_t from, idx_t to, uint64_t delimiters,
                          uint64_t terminators, uint64_t pairs, idx_t offset, vector<RowCount> &rows,
                          idx_t limit) const {
	auto &c = cursor;
	const uint64_t below_to = to == CSV_BLOCK_BYTES ? ~uint64_t(0) : (uint64_t(1) << to) - 1;
	const uint64_t range = below_to & (~uint64_t(0) << from);
	const CSVState incoming = c.states[1];
	uint64_t done = (uint64_t(1) << from) - 1;
	for (uint64_t rest = terminators & range; rest; rest &= rest - 1) {
		const auto at = static_cast<idx_t>(std::countr_zero(rest));
		const bool pair = (pairs >> at) & 1;
		const idx_t last = at + pair;
		const uint64_t through = last == CSV_BLOCK_BYTES - 1 ? ~uint64_t(0) : (uint64_t(2) << last) - 1;
		c.delimiters += static_cast<idx_t>(std::popcount(delimiters & through & ~done));
		const CSVState before = at == from ? incoming : PlainState(block, at - 1, from, incoming);
		done = through;
		if (before == CSVState::RECORD_SEPARATOR || before == CSVState::NOT_SET) {
			c.empty_lines++;
			c.line_start = offset + at + 1;
			continue;
		}
		c.states[0] = before;
		c.states[1] = pair ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR;
		CountRow(c, offset + at, 0, rows);
		if (rows.size() >= limit) {
			c.pos = offset + at + 1;
			return true;
		}
	}
	c.delimiters += static_cast<idx_t>(std::popcount(delimiters & range & ~done));
	c.states[0] = to - from >= 2 ? PlainState(block, to - 2, from, incoming) : incoming;
	c.states[1] = PlainState(block, to - 1, from, incoming);
	return false;
}

void RowLexer::CountRows(RowCountCursor &cursor, const char *data, idx_t base, idx_t end, idx_t max_rows,
                         vector<RowCount> &rows) const {
	auto &c = cursor;
	const idx_t limit = rows.size() + max_rows;
	char padded[CSV_BLOCK_BYTES];
	while (c.pos < end && !c.invalid && rows.size() < limit) {
		const idx_t size = MinValue<idx_t>(CSV_BLOCK_BYTES, end - c.pos);
		const char *block = data + (c.pos - base);
		uint64_t valid = ~uint64_t(0);
		if (size < CSV_BLOCK_BYTES) {
			memset(padded, 0, sizeof(padded));
			memcpy(padded, block, size);
			block = padded;
			valid = (uint64_t(1) << size) - 1;
		}
		const CsvVector vector(block);
		uint64_t delimiters = 0;
		uint64_t terminators = 0;
		uint64_t pairs = 0;
		uint64_t specials = valid;
		if (count_plain) {
			delimiters = vector.Equal(dialect.delimiter) & valid;
			const uint64_t line_feeds = vector.Equal('\n') & valid;
			const uint64_t carriages = vector.Equal('\r') & valid;
			pairs = count_carriage_pairs ? carriages & (line_feeds >> 1) : 0;
			const uint64_t lone_feeds = line_feeds & ~(pairs << 1);
			terminators = pairs | (count_line_feeds ? lone_feeds : 0);
			specials = (carriages & ~pairs) | (count_line_feeds ? 0 : lone_feeds);
			for (idx_t i = 0; i < count_special_count; i++) {
				specials |= vector.Equal(count_specials[i]);
			}
			specials &= valid;
		}
		uint64_t standard_ends = 0;
		uint64_t quoted_ends = 0;
		uint64_t comment_ends = 0;
		bool standard_ready = false;
		bool quoted_ready = false;
		bool comment_ready = false;
		idx_t at = 0;
		while (at < size) {
			const CSVState state = c.states[1];
			const uint64_t ahead = ~uint64_t(0) << at;
			idx_t next = at;
			if (count_plain && PlainCountState(state)) {
				const uint64_t pending = specials & ahead;
				next = pending ? static_cast<idx_t>(std::countr_zero(pending)) : size;
				if (next > at && CountPlain(c, block, at, next, delimiters, terminators, pairs, c.pos, rows, limit)) {
					return;
				}
			} else if (standard_stop_count != DConstants::INVALID_INDEX && state == CSVState::STANDARD) {
				if (!standard_ready) {
					for (idx_t i = 0; i < standard_stop_count; i++) {
						standard_ends |= vector.Equal(standard_stops[i]);
					}
					standard_ends &= valid;
					standard_ready = true;
				}
				const uint64_t pending = standard_ends & ahead;
				next = pending ? static_cast<idx_t>(std::countr_zero(pending)) : size;
				if (next > at) {
					c.states[0] = CSVState::STANDARD;
					c.states[1] = CSVState::STANDARD;
				}
			} else if (quoted_stop_count != DConstants::INVALID_INDEX &&
			           (state == CSVState::QUOTED || state == CSVState::QUOTED_NEW_LINE)) {
				if (!quoted_ready) {
					for (idx_t i = 0; i < quoted_stop_count; i++) {
						quoted_ends |= vector.Equal(quoted_stops[i]);
					}
					quoted_ends &= valid;
					quoted_ready = true;
				}
				const uint64_t pending = quoted_ends & ahead;
				next = pending ? static_cast<idx_t>(std::countr_zero(pending)) : size;
				if (next > at) {
					c.states[0] = next - at >= 2 ? CSVState::QUOTED : state;
					c.states[1] = CSVState::QUOTED;
				}
			} else if (comment_stop_count != DConstants::INVALID_INDEX && state == CSVState::COMMENT) {
				if (!comment_ready) {
					for (idx_t i = 0; i < comment_stop_count; i++) {
						comment_ends |= vector.Equal(comment_stops[i]);
					}
					comment_ends &= valid;
					comment_ready = true;
				}
				const uint64_t pending = comment_ends & ahead;
				next = pending ? static_cast<idx_t>(std::countr_zero(pending)) : size;
				if (next > at) {
					c.states[0] = next - at >= 2 ? CSVState::COMMENT : state;
					c.states[1] = CSVState::COMMENT;
				}
			}
			at = next;
			if (at == size) {
				break;
			}
			if (CountByte(c, block[at], c.pos + at, rows, limit)) {
				c.pos += at + 1;
				return;
			}
			at++;
		}
		c.pos += size;
	}
}

void RowLexer::FinishCount(RowCountCursor &cursor, vector<RowCount> &rows) const {
	auto &c = cursor;
	if (c.invalid) {
		return;
	}
	const CSVState current = c.states[1];
	if ((current == CSVState::QUOTED || current == CSVState::QUOTED_NEW_LINE) && c.states[0] != CSVState::UNQUOTED) {
		c.unterminated = true;
		return;
	}
	if (current == CSVState::RECORD_SEPARATOR || current == CSVState::CARRIAGE_RETURN || current == CSVState::NOT_SET) {
		return;
	}
	if (c.comment) {
		CountRow(c, DConstants::INVALID_INDEX, c.starts_as_comment ? ROW_COUNT_COMMENT : ROW_COUNT_MID_COMMENT, rows);
		c.comment = false;
		c.starts_as_comment = false;
		return;
	}
	CountRow(c, DConstants::INVALID_INDEX, 0, rows);
}

} // namespace duckdb
