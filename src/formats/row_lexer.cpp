#include "duckdb/formats/row_lexer.hpp"

#include "duckdb/formats/csv_simd.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

#if defined(__BMI2__)
#include <immintrin.h>
#endif

namespace duckdb {

namespace {

constexpr idx_t FIELD_SLACK = 8;
constexpr idx_t ROW_BASE_LIMIT = idx_t(1) << 20;
constexpr idx_t PREFETCH_BYTES = 1024;

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

inline uint64_t Lowest(uint64_t bits) {
	return bits & (0 - bits);
}

inline uint64_t AtOrAbove(uint64_t bit) {
	return ~(bit - 1);
}

inline uint64_t Above(uint64_t bit) {
	return ~((bit << 1) - 1);
}

inline uint64_t Highest(uint64_t bits) {
	return bits ? uint64_t(1) << (63 - std::countl_zero(bits)) : 0;
}

inline void FlagFields(uint64_t specials, uint64_t boundaries, uint64_t &pending, uint32_t *block_ends) {
	uint64_t flagged = 0;
	if (pending && boundaries) {
		flagged |= Lowest(boundaries);
	}
	for (uint64_t rest = specials; rest; rest &= rest - 1) {
		const uint64_t above = boundaries & AtOrAbove(Lowest(rest));
		if (!above) {
			break;
		}
		flagged |= Lowest(above);
	}
	if (boundaries) {
		const uint64_t top = Highest(boundaries);
		pending = (specials & ~(top | (top - 1))) != 0;
	} else {
		pending = pending || specials != 0;
	}
	for (; flagged; flagged &= flagged - 1) {
		block_ends[std::popcount(boundaries & (Lowest(flagged) - 1))] |= ROW_FIELD_FLAG;
	}
}

} // namespace

RowDialect RowDialect::DuckDB(const CsvGrammar &grammar) {
	RowDialect result;
	result.delimiter = grammar.delimiter[0];
	result.delimiter_size = grammar.delimiter_size;
	for (idx_t i = 1; i < grammar.delimiter_size; i++) {
		result.delimiter_tail[i - 1] = grammar.delimiter[i];
	}
	result.distinct_delimiter = grammar.distinct_delimiter;
	result.quote = grammar.quote;
	result.escape = grammar.escape;
	result.comment = grammar.comment;
	result.quoting = grammar.quote ? RowQuoting::FIELD_START : RowQuoting::NONE;
	result.duckdb = true;
	result.escape_in_quotes = grammar.quoted_escape && grammar.strict_mode;
	result.unquoted_escape = grammar.quoted_escape && !grammar.strict_mode;
	result.carriage_terms = true;
	result.merge_terms = true;
	result.carry_on = grammar.carry_on_newline;
	result.strict = grammar.strict_mode;
	result.reopen = grammar.quote == grammar.escape || (!grammar.strict_mode && grammar.escape != '\0');
	result.empty_space = grammar.empty_space;
	result.quoted_spaces = grammar.quote != '\0' && grammar.trim_quoted_spaces;
	result.new_line = grammar.new_line;
	return result;
}

RowDialect RowDialect::PostgresCsv(char delimiter, char quote, char escape, char line_break) {
	RowDialect result;
	result.delimiter = delimiter;
	result.quote = quote;
	result.escape = escape;
	result.quoting = RowQuoting::ANYWHERE;
	result.escape_in_quotes = escape != '\0' && escape != quote;
	result.physical_lines = true;
	result.line_break = line_break;
	result.strip_carriage = line_break == '\n';
	result.strict = true;
	return result;
}

RowDialect RowDialect::PostgresText(char delimiter, char line_break) {
	RowDialect result;
	result.delimiter = delimiter;
	result.escape = '\\';
	result.backslash = true;
	result.line_break = line_break;
	result.strip_carriage = line_break == '\n';
	result.strict = true;
	return result;
}

bool RowFields::Fits(idx_t rows_used, idx_t ends_used, idx_t more) const {
	const idx_t row_head = NumericCast<idx_t>(rows - row_store.get());
	const idx_t end_head = NumericCast<idx_t>(ends - end_store.get());
	return MaxValue(row_head + rows_used, end_head + ends_used) + more + FIELD_SLACK <= capacity;
}

void RowFields::Reserve(idx_t more) {
	if (Fits(row_count, end_count, more)) {
		return;
	}
	const idx_t needed = MaxValue(row_count, end_count) + more + FIELD_SLACK;
	if (needed <= capacity) {
		memmove(row_store.get(), rows, row_count * sizeof(uint32_t));
		memmove(end_store.get(), ends, end_count * sizeof(uint32_t));
		rows = row_store.get();
		ends = end_store.get();
		return;
	}
	const idx_t grown = MaxValue(needed, capacity * 2);
	auto new_rows = make_unsafe_uniq_array_uninitialized<uint32_t>(grown);
	auto new_ends = make_unsafe_uniq_array_uninitialized<uint32_t>(grown);
	if (row_count) {
		memcpy(new_rows.get(), rows, row_count * sizeof(uint32_t));
	}
	if (end_count) {
		memcpy(new_ends.get(), ends, end_count * sizeof(uint32_t));
	}
	row_store = std::move(new_rows);
	end_store = std::move(new_ends);
	rows = row_store.get();
	ends = end_store.get();
	capacity = grown;
}

void RowFields::Drop(idx_t count, idx_t dropped_ends) {
	D_ASSERT(count <= row_count && dropped_ends <= end_count && open_extras <= extras.size());
	rows += count;
	ends += dropped_ends;
	row_count -= count;
	end_count -= dropped_ends;
	base += count;
	while (event_head < events.size() && events[event_head].row < base) {
		event_head++;
	}
	while (anomaly_head < anomalies.size() && anomalies[anomaly_head].row < base) {
		anomaly_head++;
	}
	if (event_head == events.size()) {
		events.clear();
		event_head = 0;
	}
	if (anomaly_head == anomalies.size() && !anomalies.empty()) {
		anomalies.clear();
		anomaly_head = 0;
		extras.erase(extras.begin(), extras.end() - NumericCast<int64_t>(open_extras));
	}
	if (events.empty() && anomalies.empty()) {
		base = 0;
		return;
	}
	if (base < ROW_BASE_LIMIT) {
		return;
	}
	for (idx_t i = event_head; i < events.size(); i++) {
		events[i].row -= static_cast<uint32_t>(base);
	}
	for (idx_t i = anomaly_head; i < anomalies.size(); i++) {
		anomalies[i].row -= static_cast<uint32_t>(base);
	}
	base = 0;
}

void RowFields::Rebase(uint32_t delta) {
	for (idx_t i = 0; i < row_count; i++) {
		rows[i] -= delta;
	}
	for (idx_t i = 0; i < end_count; i++) {
		ends[i] = ((ends[i] & ROW_FIELD_MASK) - delta) | (ends[i] & ROW_FIELD_FLAG);
	}
	for (auto &extra : extras) {
		extra = ((extra & ROW_FIELD_MASK) - delta) | (extra & ROW_FIELD_FLAG);
	}
	for (idx_t i = anomaly_head; i < anomalies.size(); i++) {
		anomalies[i].end -= delta;
	}
	for (idx_t i = event_head; i < events.size(); i++) {
		events[i].position -= delta;
	}
}

const RowAnomaly *RowFields::Anomaly(idx_t row) const {
	const idx_t target = row + base;
	auto entry = std::lower_bound(anomalies.begin() + NumericCast<int64_t>(anomaly_head), anomalies.end(), target,
	                              [](const RowAnomaly &anomaly, idx_t value) { return anomaly.row < value; });
	return entry != anomalies.end() && entry->row == target ? &*entry : nullptr;
}

RowLexer::RowLexer(const RowDialect &dialect_p, idx_t columns_p, idx_t max_line_size_p, bool error_mode,
                   bool pad_rows_p, bool irregular_rows_p)
    : dialect(dialect_p), columns(columns_p), max_line_size(max_line_size_p), pad_rows(pad_rows_p),
      irregular_rows(irregular_rows_p) {
	D_ASSERT(columns >= 1);
	settle_strict = error_mode && dialect.strict;
	settle_length = error_mode;
	lean_shapes = dialect.duckdb && dialect.delimiter_size == 1 && !dialect.carry_on &&
	              (dialect.new_line == NewLineIdentifier::NOT_SET || dialect.new_line == NewLineIdentifier::SINGLE_N) &&
	              max_line_size >= CSV_BLOCK_BYTES;
	lean = lean_shapes && !dialect.comment && !dialect.escape_in_quotes && !dialect.unquoted_escape;
	patterns.resize(columns);
	for (idx_t phase = 0; phase < columns; phase++) {
		patterns[phase] = ExpectedPattern(columns, phase);
	}
}

void RowLexer::Start(RowCursor &cursor, idx_t begin, bool settled, bool check_arity) const {
	cursor = RowCursor();
	cursor.origin = begin;
	cursor.anchor = begin;
	cursor.pos = begin;
	cursor.field_start = begin;
	cursor.settled = settled || dialect.quoting == RowQuoting::NONE;
	cursor.check_arity = check_arity;
}

bool RowLexer::Feed(RowCursor &cursor, const char *data, idx_t base, idx_t end, bool last, RowFields *fields) const {
	if (fields) {
		return FeedBlocks<true>(cursor, data, base, end, last, fields);
	}
	return FeedBlocks<false>(cursor, data, base, end, last, nullptr);
}

template <bool EXTRACT, bool SHAPE>
bool RowLexer::LeanBlocks(RowCursor &cursor, const char *data, idx_t base, idx_t end, uint32_t *&rows_out,
                          uint32_t *&ends_out, RowShapes *shapes) const {
	const auto &d = dialect;
	if (cursor.maybe || cursor.in_comment || cursor.escape_carry || cursor.pos + CSV_BLOCK_BYTES > end) {
		return false;
	}
	const char escape = d.escape != d.quote ? d.escape : '\0';
	const uint64_t quote_follow_bit = d.reopen ? ~uint64_t(0) : 0;
	const bool track_fields = d.strict;
	idx_t pos = cursor.pos;
	uint64_t in_quotes = cursor.in_quotes;
	uint64_t after_delimiter = cursor.after_delimiter;
	uint64_t after_term = cursor.after_term;
	uint64_t after_closer = cursor.after_closer;
	uint64_t after_opener = cursor.after_opener;
	uint64_t start_space = cursor.start_space;
	uint64_t non_ascii = cursor.non_ascii;
	uint64_t pending_flag = cursor.pending_flag;
	idx_t phase = cursor.phase;
	idx_t rows = cursor.rows;
	idx_t lines = cursor.lines;
	idx_t open_rows = cursor.open_rows;
	idx_t last_row_end = cursor.last_row_end;
	idx_t field_start = cursor.field_start;
	bool settled = cursor.settled;
	bool quotes = cursor.quotes;
	bool ever_quoted = cursor.ever_quoted;
	const bool check_arity = cursor.check_arity;
	const bool no_quoted_newline = cursor.quoted_newline == DConstants::INVALID_INDEX;
	const idx_t origin = cursor.origin;
	const idx_t anchor = cursor.anchor;
	const uint64_t *expected = patterns.data();
	idx_t delimiters = cursor.delimiters;
	idx_t empty_lines = cursor.empty_lines;
	idx_t line_start = cursor.line_start;
	bool trailing_empty = cursor.trailing_empty;
	const auto settles = [&](bool block_settled) {
		return block_settled || (settle_length && no_quoted_newline && pos + CSV_BLOCK_BYTES - origin > max_line_size);
	};
	const auto too_long = [&](idx_t reach, bool block_settled) {
		const idx_t prior = last_row_end == DConstants::INVALID_INDEX ? origin : last_row_end;
		const idx_t limit =
		    last_row_end == DConstants::INVALID_INDEX && !block_settled ? max_line_size / 2 : max_line_size;
		return reach - prior > limit;
	};
	while (pos + CSV_BLOCK_BYTES <= end) {
		if constexpr (!EXTRACT && !SHAPE) {
			if (!(in_quotes | after_closer | after_opener)) {
				D_ASSERT(!d.comment && !escape);
				const char quote_byte = d.quote ? d.quote : '\r';
				const idx_t run_start = pos;
				bool need_first_term = cursor.first_term == DConstants::INVALID_INDEX;
				idx_t last_term = DConstants::INVALID_INDEX;
				while (pos + CSV_BLOCK_BYTES <= end) {
					const char *block = data + (pos - base);
					__builtin_prefetch(block + PREFETCH_BYTES);
					const CsvVector vector(block);
					if (vector.EqualAny('\r', quote_byte)) {
						break;
					}
					const uint64_t terms = vector.Equal('\n');
					const uint64_t delims = vector.Equal(d.delimiter);
					const uint64_t from_delims = (delims << 1) | after_delimiter;
					const uint64_t after_terms = (terms << 1) | after_term;
					const bool block_settled = settles(settled);
					const uint64_t merged = d.merge_terms && !d.empty_lines ? after_terms : 0;
					const uint64_t row_ends = terms & ~merged;
					const uint64_t line_ends = d.empty_lines ? row_ends & ~after_terms : row_ends;
					const uint64_t boundaries = delims | row_ends;
					idx_t next_phase = phase;
					if (check_arity && boundaries) {
						const idx_t count = std::popcount(boundaries);
#if defined(__BMI2__)
						const uint64_t pattern = _pext_u64(row_ends, boundaries);
#else
						uint64_t pattern = 0;
						for (uint64_t rest = row_ends; rest; rest &= rest - 1) {
							pattern |= uint64_t(1) << std::popcount(boundaries & (Lowest(rest) - 1));
						}
#endif
						const uint64_t mask = count == 64 ? ~uint64_t(0) : (uint64_t(1) << count) - 1;
						if (phase >= columns || ((pattern ^ expected[phase]) & mask)) {
							break;
						}
						next_phase = pattern ? count + std::countl_zero(pattern) - 64 : phase + count;
						if (next_phase >= columns) {
							break;
						}
					}
					const idx_t reach = line_ends ? pos + std::countr_zero(line_ends) : pos + CSV_BLOCK_BYTES;
					if (too_long(reach, block_settled)) {
						break;
					}
					non_ascii |= vector.NonAscii();
					settled = block_settled;
					phase = next_phase;
					if (track_fields) {
						const uint64_t field_bits = from_delims | (after_terms & ~(terms & merged));
						if (field_bits) {
							field_start = pos + 63 - std::countl_zero(field_bits);
						}
						if ((delims | terms) >> (CSV_BLOCK_BYTES - 1)) {
							field_start = pos + CSV_BLOCK_BYTES;
						}
					}
					if (line_ends) {
						if (last_row_end == DConstants::INVALID_INDEX) {
							cursor.first_row_end = reach;
						}
						last_row_end = pos + 63 - std::countl_zero(line_ends);
					}
					rows += std::popcount(row_ends);
					lines += std::popcount(terms);
					if (terms) {
						last_term = pos + 63 - std::countl_zero(terms);
						if (need_first_term) {
							need_first_term = false;
							cursor.first_term = pos + std::countr_zero(terms);
							cursor.first_boundary = cursor.first_term + 1;
						}
					}
					after_delimiter = delims >> (CSV_BLOCK_BYTES - 1);
					after_term = terms >> (CSV_BLOCK_BYTES - 1);
					pos += CSV_BLOCK_BYTES;
				}
				if (pos != run_start) {
					const char *last_block = data + (pos - CSV_BLOCK_BYTES - base);
					const char before_last = last_block[CSV_BLOCK_BYTES - 2];
					start_space =
					    (before_last == '\n' || before_last == d.delimiter) && last_block[CSV_BLOCK_BYTES - 1] == ' ';
					if (last_term != DConstants::INVALID_INDEX) {
						cursor.last_term = last_term;
						cursor.last_term_cr = false;
					}
				}
				if (pos + CSV_BLOCK_BYTES > end) {
					break;
				}
			}
		}
		const char *block = data + (pos - base);
		__builtin_prefetch(block + PREFETCH_BYTES);
		const CsvVector vector(block);
		if (vector.Equal('\r') || (d.comment && vector.Equal(d.comment)) || (escape && vector.Equal(escape))) {
			break;
		}
		const uint64_t raw_quote = d.quote ? vector.Equal(d.quote) : 0;
		if (in_quotes && !(raw_quote | after_opener)) {
			const bool block_settled = settles(settled);
			if (too_long(pos + CSV_BLOCK_BYTES, block_settled)) {
				break;
			}
			non_ascii |= vector.NonAscii();
			settled = block_settled;
			ever_quoted = true;
			pos += CSV_BLOCK_BYTES;
			continue;
		}
		const uint64_t line_feed = vector.Equal('\n');
		const uint64_t delimiter = vector.Equal(d.delimiter);
		const uint64_t in = (raw_quote ? CsvPrefixXor(raw_quote) : 0) ^ CsvSpread(in_quotes);
		const uint64_t terms = line_feed & ~in;
		const uint64_t delims = delimiter & ~in;
		const uint64_t from_delims = (delims << 1) | after_delimiter;
		const uint64_t after_terms = (terms << 1) | after_term;
		const uint64_t field_starts = from_delims | after_terms;
		bool block_settled = settled;
		uint64_t next_closer = after_closer;
		uint64_t next_opener = after_opener;
		if (raw_quote | after_closer | after_opener) {
			const uint64_t openers = raw_quote & in;
			const uint64_t closers = raw_quote & ~in;
			const uint64_t follow = delimiter | line_feed;
			if ((openers & ~field_starts) | (((closers << 1) | after_closer) & ~follow)) {
				break;
			}
			if (!settled && no_quoted_newline) {
				if (line_feed & in) {
					break;
				}
				const uint64_t spaces = d.quoted_spaces ? vector.Equal(' ') : 0;
				const uint64_t broken =
				    ((openers << 1) | after_opener) & ~(follow | (raw_quote & quote_follow_bit)) & ~spaces;
				block_settled = settle_strict && broken;
			}
			next_closer = closers >> (CSV_BLOCK_BYTES - 1);
			next_opener = openers >> (CSV_BLOCK_BYTES - 1);
		}
		block_settled = settles(block_settled);
		const uint64_t merged = d.merge_terms && !d.empty_lines ? after_terms : 0;
		const uint64_t row_ends = terms & ~merged;
		const uint64_t line_ends = d.empty_lines ? row_ends & ~after_terms : row_ends;
		const uint64_t row_starts = after_terms & ~(terms & merged);
		const uint64_t boundaries = delims | row_ends;
		idx_t next_phase = phase;
		if (check_arity && boundaries) {
			const idx_t count = std::popcount(boundaries);
#if defined(__BMI2__)
			const uint64_t pattern = _pext_u64(row_ends, boundaries);
#else
			uint64_t pattern = 0;
			for (uint64_t rest = row_ends; rest; rest &= rest - 1) {
				pattern |= uint64_t(1) << std::popcount(boundaries & (Lowest(rest) - 1));
			}
#endif
			const uint64_t mask = count == 64 ? ~uint64_t(0) : (uint64_t(1) << count) - 1;
			if (phase >= columns || ((pattern ^ expected[phase]) & mask)) {
				break;
			}
			next_phase = pattern ? count + std::countl_zero(pattern) - 64 : phase + count;
			if (next_phase >= columns) {
				break;
			}
		}
		const idx_t reach = line_ends ? pos + std::countr_zero(line_ends) : pos + CSV_BLOCK_BYTES;
		if (too_long(reach, block_settled)) {
			break;
		}
		uint64_t special_before = 0;
		if (SHAPE) {
			special_before = after_terms;
			if (d.quoting == RowQuoting::FIELD_START && (raw_quote | after_closer)) {
				special_before |= ((raw_quote & ~in) << 1) | after_closer;
			}
			if (d.empty_space) {
				special_before |= ((field_starts & vector.Equal(' ')) << 1) | start_space;
			}
		}
		non_ascii |= vector.NonAscii();
		settled = block_settled;
		after_closer = next_closer;
		after_opener = next_opener;
		phase = next_phase;
		start_space = (field_starts >> (CSV_BLOCK_BYTES - 1)) & (block[CSV_BLOCK_BYTES - 1] == ' ');
		quotes = quotes || raw_quote;
		ever_quoted = ever_quoted || in;
		if (SHAPE) {
			uint64_t from = ~uint64_t(0);
			for (uint64_t todo = terms; todo; todo &= todo - 1) {
				const uint64_t bit = Lowest(todo);
				const idx_t at = std::countr_zero(bit);
				const uint64_t span = from & (bit - 1);
				from = Above(bit);
				if (after_terms & bit) {
					empty_lines++;
					line_start = pos + at + 1;
					continue;
				}
				delimiters += std::popcount(delims & span);
				const idx_t stop = pos + at + 1;
				const auto flags = static_cast<uint8_t>(from_delims & bit ? ROW_COUNT_EMPTY_LAST : 0);
				shapes->rows.push_back({delimiters + 1, empty_lines, line_start, stop, flags});
				delimiters = 0;
				line_start = stop;
				if (shapes->rows.size() >= shapes->limit) {
					const bool ever_escaped = cursor.ever_escaped;
					const bool unstrict = cursor.unstrict;
					Start(cursor, stop, true, false);
					cursor.quotes = quotes;
					cursor.ever_quoted = ever_quoted;
					cursor.ever_escaped = ever_escaped;
					cursor.unstrict = unstrict;
					cursor.empty_lines = empty_lines;
					cursor.line_start = line_start;
					cursor.lines = lines;
					return true;
				}
			}
			delimiters += std::popcount(delims & from);
			const uint64_t last_bit = uint64_t(1) << (CSV_BLOCK_BYTES - 1);
			trailing_empty = (delims & last_bit) && ((from_delims & last_bit) || !(special_before & last_bit));
		}
		if (track_fields) {
			const uint64_t field_bits = from_delims | row_starts;
			if (field_bits) {
				field_start = pos + 63 - std::countl_zero(field_bits);
			}
		}
		if (line_ends) {
			if (last_row_end == DConstants::INVALID_INDEX) {
				cursor.first_row_end = reach;
			}
			last_row_end = pos + 63 - std::countl_zero(line_ends);
		}
		rows += std::popcount(row_ends);
		if (terms) {
			cursor.last_term = pos + 63 - std::countl_zero(terms);
			cursor.last_term_cr = false;
			if (cursor.first_term == DConstants::INVALID_INDEX) {
				cursor.first_term = pos + std::countr_zero(terms);
				cursor.first_boundary = cursor.first_term + 1;
			}
		}
		if (!SHAPE) {
			lines += std::popcount(terms);
		}
		if (EXTRACT) {
			const uint32_t offset = static_cast<uint32_t>(pos - anchor);
			rows_out = WritePositions(row_starts, offset, rows_out);
			uint32_t *block_ends = ends_out;
			ends_out = WritePositions(boundaries, offset, ends_out);
			if (raw_quote | pending_flag) {
				FlagFields(raw_quote, boundaries, pending_flag, block_ends);
			}
			open_rows += std::popcount(row_starts);
			open_rows -= std::popcount(row_ends);
		}
		in_quotes = (in >> (CSV_BLOCK_BYTES - 1)) & 1;
		after_delimiter = delims >> (CSV_BLOCK_BYTES - 1);
		after_term = (terms >> (CSV_BLOCK_BYTES - 1)) & 1;
		if (track_fields && (after_term | after_delimiter)) {
			field_start = pos + CSV_BLOCK_BYTES;
		}
		pos += CSV_BLOCK_BYTES;
	}
	if (pos == cursor.pos) {
		return false;
	}
	if (SHAPE) {
		cursor.delimiters = delimiters;
		cursor.empty_lines = empty_lines;
		cursor.line_start = line_start;
		cursor.trailing_empty = trailing_empty;
		cursor.reopen_last = false;
	}
	cursor.pos = pos;
	cursor.in_quotes = in_quotes;
	cursor.after_delimiter = after_delimiter;
	cursor.after_term = after_term;
	cursor.after_closer = after_closer;
	cursor.after_opener = after_opener;
	cursor.start_space = start_space;
	cursor.non_ascii = non_ascii;
	cursor.pending_flag = pending_flag;
	cursor.phase = phase;
	cursor.rows = rows;
	cursor.lines = lines;
	cursor.open_rows = open_rows;
	cursor.last_row_end = last_row_end;
	cursor.field_start = field_start;
	cursor.settled = settled;
	cursor.quotes = quotes;
	cursor.ever_quoted = ever_quoted;
	cursor.after_cr = 0;
	cursor.delimiter_tail = 0;
	cursor.partial_tail = 0;
	cursor.partial_before_last = 0;
	cursor.after_partial = 0;
	cursor.after_event = 0;
	cursor.in_comment = 0;
	return false;
}

template <bool EXTRACT, bool SHAPE>
bool RowLexer::FeedBlocks(RowCursor &cursor, const char *data, idx_t base, idx_t end, bool last, RowFields *fields,
                          RowShapes *shapes) const {
	const auto &d = dialect;
	if (!cursor.ok) {
		return false;
	}
	const bool field_start_quotes = d.quoting == RowQuoting::FIELD_START;
	const bool escapes = d.backslash || d.escape_in_quotes || d.unquoted_escape;
	const bool maybe_quotes = field_start_quotes && !d.strict && d.escape == '\0';
	const bool strict_quotes = field_start_quotes && d.strict;
	const bool strict_lines = d.duckdb && d.strict && d.new_line != NewLineIdentifier::NOT_SET;
	const bool track_fields = d.duckdb && (d.comment || d.strict);
	const bool count_errors = !EXTRACT && cursor.check_arity;
	const uint64_t quote_follow_bit = d.reopen ? ~uint64_t(0) : 0;
	auto c = cursor;
	uint32_t *rows_out = nullptr;
	uint32_t *ends_out = nullptr;
	if (EXTRACT) {
		fields->Reserve(end - c.pos + CSV_BLOCK_BYTES + 1);
		rows_out = fields->rows + fields->row_count;
		ends_out = fields->ends + fields->end_count;
	}
	auto fail = [&]() {
		c.ok = false;
		if (EXTRACT) {
			fields->row_count = static_cast<idx_t>(rows_out - fields->rows);
			fields->end_count = static_cast<idx_t>(ends_out - fields->ends);
		}
		cursor = c;
		return false;
	};
	const idx_t delimiter_size = d.delimiter_size;
	const char delimiter_byte = d.delimiter;
	const char escape_byte = escapes ? d.escape : '\n';
	const char quote_byte = d.quote ? d.quote : '\n';
	const char comment_byte = d.comment ? d.comment : '\n';
	const auto plain = [&](const CsvVector &vector) {
		if (c.escape_carry | c.maybe | c.in_comment | c.after_delimiter | c.after_term | c.after_cr | c.escaped_cr |
		    c.after_closer | c.after_opener | c.start_space | c.delimiter_tail | c.partial_tail | c.after_partial |
		    c.after_event) {
			return false;
		}
		return !(c.in_quotes ? vector.EqualAny(quote_byte, escape_byte, '\n', '\r')
		                     : vector.EqualAny(delimiter_byte, '\n', '\r', escape_byte, quote_byte, comment_byte));
	};
	char padded[CSV_BLOCK_BYTES];
	char lookahead[CSV_BLOCK_BYTES];
	while (c.pos < end) {
		if (SHAPE ? lean_shapes && !shapes->skip : lean) {
			if (LeanBlocks<EXTRACT, SHAPE>(c, data, base, end, rows_out, ends_out, shapes)) {
				cursor = c;
				return true;
			}
			if (c.pos >= end) {
				break;
			}
		}
		const idx_t size = MinValue<idx_t>(CSV_BLOCK_BYTES, end - c.pos);
		if (size < CSV_BLOCK_BYTES && !last) {
			break;
		}
		if (delimiter_size > 1 && !last && c.pos + CSV_BLOCK_BYTES + delimiter_size - 1 > end) {
			break;
		}
		const char *block = data + (c.pos - base);
		__builtin_prefetch(block + PREFETCH_BYTES);
		uint64_t valid = ~uint64_t(0);
		if (size < CSV_BLOCK_BYTES) {
			memset(padded, 0, sizeof(padded));
			memcpy(padded, block, size);
			block = padded;
			valid = (uint64_t(1) << size) - 1;
		}
		const uint64_t last_bit = uint64_t(1) << (size - 1);
		const CsvVector vector(block);
		c.non_ascii |= vector.NonAscii();
		if (size == CSV_BLOCK_BYTES && plain(vector)) {
			const bool settled = c.settled || (settle_length && c.quoted_newline == DConstants::INVALID_INDEX &&
			                                   c.pos + CSV_BLOCK_BYTES - c.origin > max_line_size);
			const idx_t prior = c.last_row_end == DConstants::INVALID_INDEX ? c.origin : c.last_row_end;
			const idx_t limit =
			    c.last_row_end == DConstants::INVALID_INDEX && !settled ? max_line_size / 2 : max_line_size;
			const bool too_long = c.pos + CSV_BLOCK_BYTES - prior > limit;
			if (!too_long || (settled && !count_errors)) {
				c.settled = settled;
				c.long_rows = c.long_rows || too_long;
				c.ever_quoted = c.ever_quoted || (d.duckdb && c.in_quotes);
				c.first_term_cr = false;
				c.partial_before_last = 0;
				if (track_fields && c.field_blank) {
					c.field_blank = vector.Equal(' ') == ~uint64_t(0);
				}
				if (EXTRACT && d.strip_carriage) {
					c.after_plain_cr = 0;
				}
				if (SHAPE) {
					c.trailing_empty = false;
					c.reopen_last = false;
				}
				c.pos += CSV_BLOCK_BYTES;
				continue;
			}
		}
		const uint64_t escaped_cr_in = c.escaped_cr;
		const uint64_t delimiter_first = vector.Equal(d.delimiter) & valid;
		uint64_t delimiter = delimiter_first;
		uint64_t partial_starts[3] = {};
		if (delimiter_size > 1) {
			memset(lookahead, 0, sizeof(lookahead));
			if (size == CSV_BLOCK_BYTES) {
				memcpy(lookahead, block + CSV_BLOCK_BYTES,
				       MinValue<idx_t>(delimiter_size - 1, end - (c.pos + CSV_BLOCK_BYTES)));
			}
			const CsvVector next(lookahead);
			for (idx_t i = 1; i < delimiter_size; i++) {
				const char byte = d.delimiter_tail[i - 1];
				const uint64_t longer =
				    delimiter & ((vector.Equal(byte) >> i) | (next.Equal(byte) << (CSV_BLOCK_BYTES - i)));
				partial_starts[i - 1] = delimiter & ~longer & valid;
				delimiter = longer;
			}
			delimiter &= valid;
		}
		const uint64_t line_feed = vector.Equal('\n') & valid;
		const uint64_t carriage_return = vector.Equal('\r') & valid;
		const uint64_t newline_bits = line_feed | carriage_return;
		const uint64_t term_bits = d.carriage_terms ? newline_bits : d.line_break == '\n' ? line_feed : carriage_return;
		const uint64_t escape_bits = escapes ? vector.Equal(d.escape) & valid : 0;
		const uint64_t raw_quote = d.quote ? vector.Equal(d.quote) & valid : 0;
		const uint64_t comment_bits = d.comment ? vector.Equal(d.comment) & valid : 0;
		const uint64_t raw_spaces = d.duckdb ? vector.Equal(' ') & valid : 0;
		const uint64_t spaces = d.quoted_spaces ? raw_spaces : 0;
		uint64_t drop_q = 0;
		uint64_t drop_e = 0;
		uint64_t drop_d = 0;
		uint64_t drop_t = 0;
		uint64_t drop_c = 0;
		uint64_t opens = 0;
		uint64_t events = 0;
		uint64_t next_maybe = 0;
		uint64_t maybe_stops = 0;
		const auto resolve_maybe = [&](uint64_t at) {
			const uint64_t rest = valid & AtOrAbove(at);
			const uint64_t others = rest & ~raw_quote;
			if (!others) {
				drop_q |= raw_quote & rest;
				next_maybe = 1;
				return;
			}
			const uint64_t stop = Lowest(others);
			drop_q |= raw_quote & rest & (stop - 1);
			if (stop & (delimiter_first | newline_bits)) {
				maybe_stops |= stop;
			} else {
				opens |= stop;
			}
		};
		if (c.maybe) {
			resolve_maybe(1);
		}
		uint64_t escape_carry;
		uint64_t escaped;
		uint64_t quote;
		uint64_t in;
		uint64_t region;
		uint64_t comment_starts;
		uint64_t hidden;
		uint64_t terms;
		uint64_t delims;
		uint64_t delimiter_tail;
		uint64_t partial;
		uint64_t partial_tail;
		uint64_t from_delims;
		uint64_t field_starts;
		uint64_t space_starts;
		uint64_t openers;
		uint64_t closers;
		uint64_t uq_skip;
		uint64_t uq_before;
		uint64_t uq_landing;
		while (true) {
			escape_carry = c.escape_carry;
			const uint64_t active_escapes = escape_bits & ~drop_e;
			escaped = escapes ? CsvFindEscaped(active_escapes, escape_carry) : 0;
			if (escapes && size < CSV_BLOCK_BYTES) {
				escape_carry = ((active_escapes & ~escaped) >> (size - 1)) & 1;
			}
			if (d.carry_on && d.unquoted_escape) {
				escaped |= (((escaped & carriage_return) << 1) | c.escaped_cr) & line_feed;
			}
			quote = raw_quote & ~drop_q & ~escaped;
			const uint64_t toggles = quote ^ opens;
			in = ((toggles ? CsvPrefixXor(toggles) : 0) ^ CsvSpread(c.in_quotes)) & valid;
			region = 0;
			comment_starts = 0;
			if (comment_bits | c.in_comment) {
				if (c.in_comment) {
					const uint64_t stop = Lowest(newline_bits);
					region = stop ? stop - 1 : valid;
				}
				for (uint64_t pending = comment_bits & ~drop_c & ~in & ~escaped & ~region; pending;
				     pending &= ~region) {
					const uint64_t start = Lowest(pending);
					const uint64_t stop = Lowest(newline_bits & Above(start));
					region |= stop ? stop - start : valid & AtOrAbove(start);
					comment_starts |= start;
				}
			}
			hidden = in | escaped | region;
			terms = term_bits & ~hidden & ~drop_t;
			delims = delimiter & ~hidden & ~drop_d;
			delimiter_tail = 0;
			if (delimiter_size > 1 && !d.distinct_delimiter) {
				uint64_t covered = c.delimiter_tail;
				for (idx_t i = 1; i < delimiter_size; i++) {
					covered |= delims << i;
				}
				if (delims & covered) {
					uint64_t kept = 0;
					covered = c.delimiter_tail;
					for (uint64_t rest = delims; rest; rest &= rest - 1) {
						const uint64_t bit = Lowest(rest);
						if (bit & covered) {
							continue;
						}
						kept |= bit;
						for (idx_t i = 1; i < delimiter_size; i++) {
							covered |= bit << i;
						}
					}
					delims = kept;
				}
				for (idx_t i = 1; i < delimiter_size; i++) {
					delimiter_tail |= delims >> (CSV_BLOCK_BYTES - i);
				}
			}
			const uint64_t lone_next =
			    d.duckdb && d.carry_on ? (((carriage_return & terms) << 1) | c.after_cr) & ~line_feed & valid : 0;
			partial = 0;
			partial_tail = 0;
			if (delimiter_size > 1 && d.duckdb) {
				uint64_t covered = c.delimiter_tail;
				for (idx_t i = 0; i < delimiter_size; i++) {
					covered |= delims << i;
				}
				partial = c.partial_tail;
				const uint64_t start_ok = ~in & ~escaped & ~region & ~covered & ~lone_next;
				for (idx_t i = 1; i < delimiter_size; i++) {
					const uint64_t starts = partial_starts[i - 1] & start_ok;
					for (idx_t j = 0; j < i; j++) {
						partial |= starts << j;
						partial_tail |= j ? starts >> (CSV_BLOCK_BYTES - j) : 0;
					}
				}
				partial &= valid;
			}
			const uint64_t after_partial = (partial << 1) | c.after_partial;
			from_delims = ((delims << delimiter_size) | c.after_delimiter) & valid;
			field_starts = from_delims | (((terms << 1) | c.after_term) & valid);
			space_starts = d.empty_space ? (((field_starts & raw_spaces) << 1) | c.start_space) & valid : 0;
			openers = quote & in;
			closers = quote & ~in;
			uq_skip = 0;
			uq_before = 0;
			uq_landing = 0;
			if (field_start_quotes && (quote | c.after_closer)) {
				uq_skip = d.strict ? spaces : valid & ~(delimiter_first | newline_bits | raw_quote | comment_bits);
				const uint64_t uq_starts = ((closers << 1) | c.after_closer) & valid;
				const uint64_t uq_runs = (uq_starts & uq_skip) + uq_skip;
				uq_landing = ((uq_starts & ~uq_skip) | (uq_runs & ~uq_skip)) & valid;
				uq_before = (uq_skip & ~uq_runs) | uq_landing;
			}
			uint64_t violation = 0;
			int kind = 0;
			const auto consider = [&](uint64_t bits, int candidate) {
				bits &= valid;
				if (bits && (!violation || Lowest(bits) < violation)) {
					violation = Lowest(bits);
					kind = candidate;
				}
			};
			if (escapes && !d.backslash) {
				const uint64_t escapers = active_escapes & ~escaped;
				consider(escapers & (d.escape_in_quotes ? ~in : (uq_before | region | (after_partial & ~in))), 1);
			}
			if (region) {
				consider(quote & region, 2);
				consider(comment_starts & after_partial, 8);
			}
			if (field_start_quotes) {
				consider(openers & ~(field_starts | space_starts | uq_landing), 2);
				if (strict_quotes) {
					const uint64_t follows =
					    delimiter_first | newline_bits | comment_bits | (raw_quote & quote_follow_bit);
					consider(uq_landing & ~follows & ~events, 3);
				} else if (maybe_quotes) {
					consider(uq_landing & quote, 4);
				}
			}
			if (strict_lines) {
				uint64_t wrong = line_feed;
				if (d.carry_on) {
					wrong = line_feed & ~(((carriage_return & terms) << 1) | c.after_cr);
				} else if (d.new_line == NewLineIdentifier::SINGLE_N) {
					wrong = carriage_return;
				}
				const uint64_t before_ok = from_delims | space_starts | (region << 1) | c.in_comment | after_partial |
				                           (c.file_start && c.pos == c.origin ? 1 : 0);
				consider(wrong & ~hidden & ~drop_t & ~before_ok & ~events, 5);
			}
			if (lone_next) {
				if (d.strict) {
					const uint64_t fine = raw_quote | comment_bits | (d.empty_space ? raw_spaces : 0);
					consider(lone_next & ~fine & ~events, 6);
				} else {
					consider(lone_next & delims, 7);
				}
			}
			if (!violation) {
				break;
			}
			if (!c.settled || (count_errors && (kind == 3 || kind == 5 || kind == 6))) {
				return fail();
			}
			switch (kind) {
			case 1:
				drop_e |= violation;
				break;
			case 2:
				drop_q |= violation;
				break;
			case 3: {
				events |= violation;
				const uint64_t stop = Lowest((delimiter | newline_bits | comment_bits) & Above(violation));
				drop_q |= raw_quote & (stop ? stop - violation : valid & AtOrAbove(violation));
				break;
			}
			case 4:
				resolve_maybe(violation);
				break;
			case 5:
				events |= violation;
				drop_t |= violation;
				break;
			case 6:
				events |= violation;
				drop_t |= violation & carriage_return;
				drop_d |= violation & delims;
				break;
			case 7:
				drop_d |= violation;
				break;
			default:
				drop_c |= violation;
				break;
			}
		}
		c.escape_carry = escape_carry;
		if (d.carry_on && d.unquoted_escape) {
			c.escaped_cr = ((escaped & carriage_return) >> (size - 1)) & 1;
		}
		c.maybe = next_maybe;
		const uint64_t comment_ends = terms & ((region << 1) | c.in_comment);
		c.in_comment = (region >> (size - 1)) & 1;
		const uint64_t carriage = carriage_return & terms;
		const uint64_t feeds = line_feed & terms;
		if (d.escape_in_quotes || d.unquoted_escape) {
			c.doubled = c.doubled || (escape_bits & ~drop_e & in) != 0;
		}
		if (field_start_quotes && (quote | c.after_closer | c.after_opener)) {
			const uint64_t ends_field = delimiter | line_feed | carriage_return;
			const uint64_t follow = ends_field | (quote & quote_follow_bit);
			c.doubled = c.doubled || (uq_landing & quote) != 0 || (d.escape != '\0' && (uq_before & uq_skip) != 0);
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
			c.after_closer = ((closers | (uq_before & uq_skip)) >> (size - 1)) & 1;
			c.after_opener = (openers >> (size - 1)) & 1;
		}
		c.start_space = ((field_starts & raw_spaces) >> (size - 1)) & 1;
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
		uint64_t row_ends = terms & ~merged & ~comment_ends;
		const uint64_t line_ends = (d.empty_lines ? row_ends & ~after_terms : row_ends) | comment_ends;
		uint64_t row_starts = after_terms & ~(terms & merged) & ~region & valid;
		const idx_t events_from = EXTRACT ? fields->events.size() : 0;
		const idx_t rows_before = EXTRACT ? static_cast<idx_t>(rows_out - fields->rows) : 0;
		if (EXTRACT && events) {
			for (uint64_t pending = events; pending; pending &= pending - 1) {
				const uint64_t at = Lowest(pending);
				const uint64_t upto = (at - 1) | at;
				const uint64_t row_bits = row_starts & upto;
				const idx_t row = rows_before + std::popcount(row_bits);
				if (row == 0) {
					continue;
				}
				const idx_t column = row_bits ? std::popcount(delims & (at - 1) & AtOrAbove(Highest(row_bits)))
				                              : c.phase + std::popcount(delims & (at - 1));
				const uint64_t field_bits = (from_delims | row_starts) & upto;
				const idx_t position = field_bits ? c.pos + std::countr_zero(Highest(field_bits)) : c.field_start;
				fields->events.push_back({static_cast<uint32_t>(row - 1 + fields->base), static_cast<uint32_t>(column),
				                          static_cast<uint32_t>(position - c.anchor)});
			}
		}
		const uint64_t first_event = SHAPE && !shapes->skip ? Lowest(events) : 0;
		const uint64_t before_event = first_event ? first_event - 1 : valid;
		if (d.duckdb) {
			c.ever_quoted = c.ever_quoted || (in & before_event) != 0;
			if (d.escape != '\0') {
				c.ever_escaped = c.ever_escaped || (uq_landing & quote & before_event) != 0 ||
				                 (escaped & in & before_event & (d.strict ? raw_quote : valid)) != 0;
			}
			if (d.unquoted_escape) {
				c.unstrict = c.unstrict || (escape_bits & ~drop_e & ~escaped & ~in & before_event) != 0;
			}
		}
		if (SHAPE) {
			const bool skip = shapes->skip;
			const uint64_t escaped_returns =
			    d.carry_on ? ((escaped & carriage_return & ~in) << 1) | escaped_cr_in : uint64_t(0);
			uint64_t line_comments = after_terms | space_starts | uq_before | escaped_returns;
			if (skip) {
				line_comments |= from_delims | (((events << 1) | c.after_event) & valid);
			}
			uint64_t from = valid;
			for (uint64_t todo = (terms | comment_starts) & before_event; todo; todo &= todo - 1) {
				const uint64_t bit = Lowest(todo);
				const idx_t at = std::countr_zero(bit);
				const uint64_t span = from & (bit - 1);
				if (bit & comment_starts) {
					c.delimiters += std::popcount(delims & span);
					from = AtOrAbove(bit);
					c.comment = true;
					c.comment_line = (line_comments & bit) != 0;
					continue;
				}
				from = Above(bit);
				const bool after_carriage = d.carry_on && (((carriage << 1) | c.after_cr) & bit);
				if (skip && !(after_carriage && (line_feed & bit))) {
					c.lines++;
				}
				if (after_terms & bit) {
					if (after_carriage) {
						continue;
					}
					c.empty_lines++;
					c.line_start = c.pos + at + 1;
					if (!skip) {
						continue;
					}
					ShapeRow(c, c.pos + at, 0, shapes->rows);
				} else {
					c.delimiters += std::popcount(delims & span);
					uint8_t flags = 0;
					if (c.comment) {
						flags = c.comment_line ? ROW_COUNT_COMMENT : ROW_COUNT_MID_COMMENT;
					} else if (from_delims & bit) {
						flags = ROW_COUNT_EMPTY_LAST;
					}
					if (skip && c.comment && c.comment_line) {
						c.comment = false;
						c.comment_line = false;
						c.delimiters = 0;
						c.line_start = c.pos + at + 1;
						continue;
					}
					ShapeRow(c, c.pos + at, flags, shapes->rows);
				}
				if (shapes->rows.size() >= shapes->limit) {
					const bool carriage_end = (carriage & bit) != 0;
					const idx_t stop = c.pos + at + 1;
					const bool quotes = c.quotes;
					const bool ever_quoted = c.ever_quoted;
					const bool ever_escaped = c.ever_escaped;
					const bool unstrict = c.unstrict;
					const idx_t empty_lines = c.empty_lines;
					const idx_t line_start = c.line_start;
					const idx_t lines = c.lines;
					Start(c, stop, true, false);
					c.after_cr = carriage_end;
					c.quotes = quotes;
					c.ever_quoted = ever_quoted;
					c.ever_escaped = ever_escaped;
					c.unstrict = unstrict;
					c.empty_lines = empty_lines;
					c.line_start = line_start;
					c.lines = lines;
					cursor = c;
					return true;
				}
			}
			if (first_event) {
				c.invalid = true;
				cursor = c;
				return true;
			}
			c.delimiters += std::popcount(delims & from & valid);
			const uint64_t special_before = after_terms | uq_before | space_starts | maybe_stops | escaped_returns;
			c.trailing_empty = delimiter_size == 1 && (delims & last_bit) &&
			                   ((from_delims & last_bit) || !(special_before & last_bit));
			c.reopen_last = (uq_landing & quote & last_bit) != 0;
		}
		const uint64_t mid_comments = SHAPE ? 0 : comment_starts & ~after_terms;
		if (mid_comments) {
			uint64_t dropped = 0;
			const uint64_t all_starts = row_starts;
			idx_t dropped_rows = 0;
			for (uint64_t pending = mid_comments; pending; pending &= pending - 1) {
				const uint64_t start = Lowest(pending);
				const uint64_t below = start - 1;
				const uint64_t row_bits = row_starts & below;
				const uint64_t field_bits = (from_delims | row_starts) & (below | start);
				const idx_t phase = row_bits ? std::popcount(delims & below & AtOrAbove(Highest(row_bits)))
				                             : c.phase + std::popcount(delims & below);
				const bool empty = (field_bits & start) != 0;
				const uint64_t field = field_bits ? below & AtOrAbove(Highest(field_bits)) : below;
				const bool blank = !empty && !(field & ~raw_spaces) && (field_bits || c.field_blank);
				if (!blank) {
					if (!empty || phase > 0) {
						row_ends |= start;
					}
					continue;
				}
				idx_t row_index = rows_before + std::popcount(all_starts & below) - dropped_rows;
				if (row_bits) {
					const uint64_t row_start = Highest(row_bits);
					dropped |= below & AtOrAbove(row_start);
					row_starts &= ~row_start;
				} else {
					dropped |= below;
					if (EXTRACT) {
						rows_out--;
						ends_out -= MinValue(c.phase, columns);
						fields->extras.resize(fields->extras.size() - fields->open_extras);
						fields->open_extras = 0;
						c.open_rows--;
					}
					c.phase = 0;
				}
				row_index--;
				dropped_rows++;
				if (EXTRACT) {
					for (idx_t i = events_from; i < fields->events.size(); i++) {
						if (fields->events[i].row > row_index + fields->base) {
							fields->events[i].row--;
						}
					}
				}
			}
			delims &= ~dropped;
		}
		if (track_fields) {
			const uint64_t field_bits = (from_delims | row_starts) & valid;
			if (field_bits) {
				const uint64_t top = Highest(field_bits);
				c.field_start = c.pos + std::countr_zero(top);
				c.field_blank = !(valid & AtOrAbove(top) & ~raw_spaces);
			} else {
				c.field_blank = c.field_blank && !(valid & ~raw_spaces);
			}
		}
		const uint64_t boundaries = delims | row_ends;
		idx_t pad_count = 0;
		idx_t pad_total = 0;
		uint8_t pad_at[CSV_BLOCK_BYTES];
		uint32_t pad_size[CSV_BLOCK_BYTES];
		bool irregular = false;
		uint64_t row_pattern = 0;
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
			if (c.phase < columns && !((pattern ^ patterns[c.phase]) & mask)) {
				c.phase = pattern ? count + std::countl_zero(pattern) - 64 : c.phase + count;
			} else if (EXTRACT && irregular_rows) {
				irregular = true;
				row_pattern = pattern;
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
			if (!irregular && c.phase >= columns) {
				return fail();
			}
		}
		const idx_t prior = c.last_row_end == DConstants::INVALID_INDEX ? c.origin : c.last_row_end;
		const idx_t limit =
		    c.last_row_end == DConstants::INVALID_INDEX && !c.settled ? max_line_size / 2 : max_line_size;
		const idx_t reach = line_ends ? c.pos + std::countr_zero(line_ends) : c.pos + size;
		if (reach - prior > limit || max_line_size < CSV_BLOCK_BYTES) {
			if (!c.settled || count_errors) {
				return fail();
			}
			c.long_rows = true;
		}
		if (line_ends) {
			if (c.last_row_end == DConstants::INVALID_INDEX) {
				c.first_row_end = reach;
			}
			c.last_row_end = c.pos + 63 - std::countl_zero(line_ends);
		}
		c.rows += std::popcount(row_ends);
		if (c.first_term_cr) {
			c.first_term_cr = false;
			c.first_boundary += line_feed & 1;
		}
		if (terms) {
			const idx_t last = 63 - std::countl_zero(terms);
			c.last_term = c.pos + last;
			c.last_term_cr = (carriage_return >> last) & 1;
			if (c.first_term == DConstants::INVALID_INDEX) {
				const idx_t first = std::countr_zero(terms);
				c.first_term = c.pos + first;
				c.first_boundary = c.first_term + 1;
				if (d.carry_on && ((carriage_return >> first) & 1)) {
					if (first + 1 < size) {
						c.first_boundary += (line_feed >> (first + 1)) & 1;
					} else {
						c.first_term_cr = true;
					}
				}
			}
		}
		if (!SHAPE) {
			c.lines += std::popcount(d.physical_lines ? (d.line_break == '\n' ? line_feed : carriage_return) : terms);
			if (d.carry_on) {
				c.lines -= std::popcount(feeds & ((carriage << 1) | c.after_cr));
			}
		}
		if (EXTRACT) {
			const uint32_t offset = static_cast<uint32_t>(c.pos - c.anchor);
			rows_out = WritePositions(row_starts, offset, rows_out);
			c.open_rows += std::popcount(row_starts);
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
				FlagFields(specials, boundaries, c.pending_flag, block_ends);
			}
			if (irregular) {
				const idx_t first_end = static_cast<idx_t>(block_ends - fields->ends);
				const idx_t written = static_cast<idx_t>(ends_out - block_ends);
				uint32_t raw[CSV_BLOCK_BYTES + 1];
				memcpy(raw, block_ends, written * sizeof(uint32_t));
				const idx_t rows_used = static_cast<idx_t>(rows_out - fields->rows);
				const idx_t more = written * columns + (end - c.pos) + CSV_BLOCK_BYTES;
				if (!fields->Fits(rows_used, first_end, more)) {
					fields->row_count = rows_used;
					fields->end_count = first_end;
					fields->Reserve(more);
					rows_out = fields->rows + rows_used;
				}
				uint32_t *out = fields->ends + first_end;
				for (idx_t i = 0; i < written; i++) {
					if ((row_pattern >> i) & 1) {
						EndRow(c, *fields, raw[i], rows_used, out);
						continue;
					}
					if (c.phase < columns) {
						*out++ = raw[i];
					} else if (d.strict) {
						fields->extras.push_back(raw[i]);
						fields->open_extras++;
					}
					c.phase++;
				}
				ends_out = out;
			} else if (pad_count) {
				const idx_t first_end = static_cast<idx_t>(block_ends - fields->ends);
				const idx_t written = static_cast<idx_t>(ends_out - block_ends);
				const idx_t rows_used = static_cast<idx_t>(rows_out - fields->rows);
				const idx_t more = pad_total + (end - c.pos) + CSV_BLOCK_BYTES;
				if (!fields->Fits(rows_used, first_end + written, more)) {
					fields->row_count = rows_used;
					fields->end_count = first_end + written;
					fields->Reserve(more);
					rows_out = fields->rows + rows_used;
				}
				uint32_t *const block_start = fields->ends + first_end;
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
			if (!irregular) {
				c.open_rows -= std::popcount(row_ends);
			}
		}
		c.in_quotes = (in >> (size - 1)) & 1;
		c.after_delimiter = size < delimiter_size ? 0 : delims >> (size - delimiter_size);
		c.after_term = (terms >> (size - 1)) & 1;
		c.after_cr = (carriage >> (size - 1)) & 1;
		c.delimiter_tail = delimiter_tail;
		c.partial_tail = partial_tail;
		c.partial_before_last = size > 1 ? (partial >> (size - 2)) & 1 : c.after_partial;
		c.after_partial = (partial >> (size - 1)) & 1;
		c.after_event = (events >> (size - 1)) & 1;
		if (track_fields && (c.after_term | c.after_delimiter)) {
			c.field_start = c.pos + size + (c.after_term ? 0 : std::countr_zero(c.after_delimiter));
			c.field_blank = true;
		}
		c.pos += size;
	}
	if (EXTRACT) {
		fields->row_count = static_cast<idx_t>(rows_out - fields->rows);
		fields->end_count = static_cast<idx_t>(ends_out - fields->ends);
	}
	cursor = c;
	return true;
}

void RowLexer::EndRow(RowCursor &c, RowFields &fields, uint32_t entry, idx_t rows_written, uint32_t *&out) const {
	const auto row = static_cast<uint32_t>(rows_written - c.open_rows + fields.base);
	c.open_rows--;
	const idx_t field_count = c.phase + 1;
	c.phase = 0;
	if (field_count == columns) {
		*out++ = entry;
		return;
	}
	if (field_count < columns) {
		*out++ = entry;
		std::fill_n(out, columns - field_count, entry | ROW_FIELD_FLAG);
		out += columns - field_count;
		c.padded = true;
		if (!pad_rows) {
			fields.anomalies.push_back({row, static_cast<uint32_t>(field_count), entry & ROW_FIELD_MASK, 0});
		}
		return;
	}
	if (!dialect.strict) {
		c.extras_dropped = true;
		return;
	}
	const auto first_extra = static_cast<uint32_t>(fields.extras.size() - fields.open_extras);
	fields.extras.push_back(entry);
	fields.open_extras = 0;
	fields.anomalies.push_back({row, static_cast<uint32_t>(field_count), entry & ROW_FIELD_MASK, first_extra});
}

bool RowLexer::Finish(RowCursor &cursor, const char *data, idx_t base, idx_t end, bool eof, RowFields *fields,
                      CSVState origin_state, CsvRangeEntry &entry) const {
	auto &c = cursor;
	if (!c.ok || c.pos != end) {
		c.ok = false;
		return false;
	}
	const bool ends_with_term = end > c.origin && c.last_term == end - 1 && !c.in_quotes;
	entry = CsvRangeEntry();
	entry.computed = true;
	entry.end.states[0] = origin_state;
	entry.end.states[1] = origin_state;
	if (c.in_quotes) {
		entry.end.states[1] = CSVState::QUOTED;
	} else if (ends_with_term) {
		const bool carriage_end = dialect.carry_on && c.last_term_cr;
		entry.end.states[1] = carriage_end ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR;
	} else if (end != c.origin) {
		entry.end.states[1] = CSVState::STANDARD;
	}
	if (c.last_term != DConstants::INVALID_INDEX) {
		entry.end.last_position = c.last_term + 1;
	}
	entry.end.line_end = c.last_row_end;
	if (c.first_term != DConstants::INVALID_INDEX) {
		entry.first_boundary = c.first_boundary;
		entry.boundary.states[1] = CSVState::RECORD_SEPARATOR;
		entry.boundary.last_position = entry.first_boundary;
	}
	if (!eof) {
		return true;
	}
	bool unfinished = false;
	if (!dialect.duckdb) {
		c.escape_carry = 0;
		unfinished = c.in_quotes != 0;
	} else if (c.in_quotes && !c.escape_carry) {
		if (!dialect.strict) {
			c.unstrict = true;
		} else if (fields || !c.check_arity) {
			unfinished = true;
		} else {
			c.ok = false;
			return false;
		}
	}
	if (!ends_with_term) {
		if (end != c.origin) {
			const idx_t limit =
			    c.last_row_end == DConstants::INVALID_INDEX && !c.settled ? max_line_size / 2 : max_line_size;
			const idx_t prior = c.last_row_end == DConstants::INVALID_INDEX ? c.origin : c.last_row_end;
			const bool row = !c.in_comment;
			const bool irregular = row && fields && c.check_arity && irregular_rows;
			if (row && c.check_arity && !pad_rows && !irregular && c.phase != columns - 1 && !unfinished) {
				c.ok = false;
				return false;
			}
			if (end - prior > limit) {
				if (!c.settled || (!fields && c.check_arity)) {
					c.ok = false;
					return false;
				}
				c.long_rows = true;
			}
			bool event = unfinished;
			if (!unfinished && row && dialect.duckdb && dialect.strict && dialect.quote && c.partial_before_last &&
			    c.field_start >= base && c.field_start < end) {
				const char *field = data + (c.field_start - base);
				event = field[0] == dialect.quote || (dialect.empty_space && field[0] == ' ' &&
				                                      c.field_start + 1 < end && field[1] == dialect.quote);
				if (event && !fields && c.check_arity) {
					c.ok = false;
					return false;
				}
			}
			if (event && fields) {
				fields->events.push_back({static_cast<uint32_t>(fields->row_count - c.open_rows + fields->base),
				                          static_cast<uint32_t>(c.phase),
				                          static_cast<uint32_t>(c.field_start - c.anchor)});
			}
			c.unfinished = unfinished;
			c.lines += !unfinished;
			c.unterminated = true;
			c.rows += row;
			const idx_t pads = row && c.check_arity && pad_rows && !irregular ? columns - 1 - c.phase : 0;
			c.padded = c.padded || pads;
			if (row && fields) {
				fields->Reserve(columns + pads);
				auto position = static_cast<uint32_t>(end - c.anchor);
				if (dialect.strip_carriage && c.after_plain_cr) {
					position--;
				}
				if (c.pending_flag) {
					position |= ROW_FIELD_FLAG;
				}
				if (irregular) {
					uint32_t *out = fields->ends + fields->end_count;
					EndRow(c, *fields, position, fields->row_count, out);
					fields->end_count = static_cast<idx_t>(out - fields->ends);
				} else {
					fields->ends[fields->end_count++] = position;
					for (idx_t i = 0; i < pads; i++) {
						fields->ends[fields->end_count++] = position | ROW_FIELD_FLAG;
					}
				}
			}
		}
	} else if (c.check_arity && c.phase != 0) {
		c.ok = false;
		return false;
	}
	return true;
}

idx_t RowLexer::CountLines(const char *data, idx_t base, idx_t begin, idx_t end) const {
	RowCursor counter;
	Start(counter, begin, true, false);
	counter.ok = true;
	FeedBlocks<false>(counter, data, base, end, true, nullptr);
	return counter.lines;
}

void RowLexer::ShapeRow(RowCursor &cursor, idx_t end, uint8_t flags, vector<RowCount> &rows) const {
	const idx_t stop = end == DConstants::INVALID_INDEX ? end : end + 1;
	rows.push_back({cursor.delimiters + 1, cursor.empty_lines, cursor.line_start, stop, flags});
	cursor.delimiters = 0;
	cursor.comment = false;
	cursor.comment_line = false;
	if (end != DConstants::INVALID_INDEX) {
		cursor.line_start = stop;
	}
}

RowSkip::RowSkip(const RowDialect &dialect, idx_t rows_p, idx_t start_p)
    : lexer(dialect, 1, NumericLimits<idx_t>::Maximum(), false), window_start(start_p), rows(rows_p) {
	lexer.Start(cursor, start_p, true, false);
	cursor.ok = true;
	cursor.file_start = true;
	if (rows == 0) {
		start = start_p;
	}
}

bool RowSkip::Feed(const char *data, idx_t base, idx_t end, bool last) {
	if (start != DConstants::INVALID_INDEX) {
		return true;
	}
	const idx_t window_end = window_start + window.size();
	if (end > window_end) {
		const idx_t from = MaxValue(base, window_end);
		window.insert(window.end(), data + (from - base), data + (end - base));
	}
	const idx_t available = window_start + window.size();
	RowShapes shapes {skipped, rows, true};
	lexer.CountRows(cursor, window.data(), window_start, available, last, shapes);
	if (skipped.size() < rows) {
		window.erase(window.begin(), window.begin() + static_cast<std::ptrdiff_t>(cursor.pos - window_start));
		window_start = cursor.pos;
		return false;
	}
	const bool pair = lexer.Dialect().carry_on && cursor.after_cr;
	if (pair && cursor.pos == available && !last) {
		return false;
	}
	start = cursor.pos;
	if (pair && start < available && window[start - window_start] == '\n') {
		start++;
	}
	return true;
}

void RowLexer::CountRows(RowCursor &cursor, const char *data, idx_t base, idx_t end, bool last,
                         RowShapes &shapes) const {
	if (!cursor.invalid && shapes.rows.size() < shapes.limit) {
		FeedBlocks<false, true>(cursor, data, base, end, last, nullptr, &shapes);
	}
}

void RowLexer::FinishCount(RowCursor &cursor, vector<RowCount> &rows) const {
	auto &c = cursor;
	if (c.invalid) {
		return;
	}
	if (c.in_quotes && !c.reopen_last && !c.escape_carry) {
		c.unterminated = true;
		return;
	}
	if (c.pos == c.origin || c.after_term) {
		return;
	}
	uint8_t flags = 0;
	if (c.comment) {
		flags = c.comment_line ? ROW_COUNT_COMMENT : ROW_COUNT_MID_COMMENT;
	} else if (c.trailing_empty) {
		flags = ROW_COUNT_EMPTY_LAST;
	}
	ShapeRow(c, DConstants::INVALID_INDEX, flags, rows);
}

} // namespace duckdb
