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

} // namespace

RowDialect RowDialect::DuckDB(const CsvGrammar &grammar) {
	RowDialect result;
	result.delimiter = grammar.delimiter[0];
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
	result.new_line = grammar.new_line;
	result.supported = grammar.delimiter_size == 1 && !grammar.only_crlf;
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

RowLexer::RowLexer(const RowDialect &dialect_p, idx_t columns_p, idx_t max_line_size_p, bool error_mode)
    : dialect(dialect_p), columns(columns_p), max_line_size(max_line_size_p) {
	const idx_t min_columns = dialect.merge_terms ? 2 : 1;
	eligible = dialect.supported && columns >= min_columns && max_line_size >= CSV_BLOCK_BYTES;
	settle_strict = error_mode && dialect.strict;
	settle_length = error_mode;
	if (eligible) {
		patterns.resize(columns);
		for (idx_t phase = 0; phase < columns; phase++) {
			patterns[phase] = ExpectedPattern(columns, phase);
		}
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
	char padded[CSV_BLOCK_BYTES];
	while (c.pos < end) {
		const idx_t size = MinValue<idx_t>(CSV_BLOCK_BYTES, end - c.pos);
		if (size < CSV_BLOCK_BYTES && !last) {
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
		uint64_t rejected = 0;
		if (d.reject_escape) {
			rejected |= vector.Equal(d.escape);
		}
		if (d.comment) {
			rejected |= vector.Equal(d.comment);
		}
		if (rejected & valid) {
			return fail();
		}
		const uint64_t delimiter = vector.Equal(d.delimiter);
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
		const uint64_t delims = delimiter & ~hidden & valid;
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
		if (field_start_quotes && (quote | c.after_closer | c.after_opener)) {
			const uint64_t openers = quote & in;
			const uint64_t closers = quote & ~in;
			const uint64_t starts = ((delims | terms) << 1) | c.after_separator;
			const uint64_t reopened = (closers << 1) | c.after_closer;
			const uint64_t allowed = d.reopen ? reopened : 0;
			if (openers & ~starts & ~allowed) {
				return fail();
			}
			const uint64_t follow = delimiter | line_feed | carriage_return | (quote & quote_follow_bit);
			if (reopened & ~follow & valid) {
				return fail();
			}
			c.doubled = c.doubled || (reopened & quote) != 0;
			if (!c.settled && c.quoted_newline == DConstants::INVALID_INDEX) {
				const uint64_t quoted_newlines = (line_feed | carriage_return) & in & valid;
				const idx_t limit =
				    quoted_newlines ? c.pos + std::countr_zero(quoted_newlines) : NumericLimits<idx_t>::Maximum();
				const uint64_t broken = ((openers << 1) | c.after_opener) & ~follow & valid;
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
		c.quotes = c.quotes || raw_quote != 0;
		if (!c.settled && settle_length && c.quoted_newline == DConstants::INVALID_INDEX &&
		    c.pos + size - c.origin > max_line_size) {
			c.settled = true;
		}
		const uint64_t row_ends = d.merge_terms ? terms & ~((terms << 1) | c.after_term) : terms;
		const uint64_t boundaries = delims | row_ends;
		if (c.check_arity && boundaries) {
			const idx_t count = std::popcount(boundaries);
#if defined(__BMI2__)
			const uint64_t pattern = _pext_u64(row_ends, boundaries);
			const uint64_t mask = count == 64 ? ~uint64_t(0) : (uint64_t(1) << count) - 1;
			if ((pattern ^ patterns[c.phase]) & mask) {
				return fail();
			}
			c.phase = pattern ? count + std::countl_zero(pattern) - 64 : c.phase + count;
#else
			idx_t expect = columns - 1 - c.phase;
			idx_t next_phase = c.phase + count;
			for (uint64_t rows = row_ends; rows; rows &= rows - 1) {
				const uint64_t below = (rows & (0 - rows)) - 1;
				const idx_t index = std::popcount(boundaries & below);
				if (index != expect) {
					return fail();
				}
				expect += columns;
				next_phase = count - index - 1;
			}
			c.phase = next_phase;
#endif
			if (c.phase >= columns) {
				return fail();
			}
		}
		const idx_t first_limit = max_line_size / 2;
		if (row_ends) {
			const idx_t first = c.pos + std::countr_zero(row_ends);
			if (c.last_row_end == DConstants::INVALID_INDEX) {
				if (first - c.origin > first_limit) {
					return fail();
				}
				c.first_row_end = first;
			} else if (first - c.last_row_end > max_line_size) {
				return fail();
			}
			c.last_row_end = c.pos + 63 - std::countl_zero(row_ends);
			c.rows += std::popcount(row_ends);
		} else if (c.last_row_end == DConstants::INVALID_INDEX) {
			if (c.pos + size - c.origin > first_limit) {
				return fail();
			}
		} else if (c.pos + size - c.last_row_end > max_line_size) {
			return fail();
		}
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
			const uint64_t row_starts = (d.merge_terms ? follows & ~terms : follows) & valid;
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
		}
		c.in_quotes = (in >> (size - 1)) & 1;
		c.after_separator = ((delims | terms) >> (size - 1)) & 1;
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
			if (!eof || (c.check_arity && c.phase != columns - 1) || end - prior > limit) {
				c.ok = false;
				return false;
			}
			c.rows++;
			c.lines++;
			c.unterminated = true;
			if (fields) {
				fields->Reserve(1);
				auto position = static_cast<uint32_t>(end - c.origin);
				if (dialect.strip_carriage && c.after_plain_cr) {
					position--;
				}
				if (c.pending_flag) {
					position |= ROW_FIELD_FLAG;
				}
				fields->ends[fields->end_count++] = position;
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

} // namespace duckdb
