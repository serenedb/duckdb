#include "duckdb/formats/csv_index.hpp"

#include "duckdb/formats/csv_simd.hpp"

namespace duckdb {

namespace {

constexpr uint64_t EVEN_BITS = 0x5555555555555555ULL;
constexpr uint64_t ODD_BITS = ~EVEN_BITS;

uint64_t Fill(uint64_t from, uint64_t until) {
	return ((until - from) | from) & ~until;
}

uint64_t Hold(uint64_t value, uint64_t at, uint64_t initial) {
	uint64_t from = at & value;
	if (initial && !(at & 1)) {
		from |= 1;
	}
	return Fill(from, at & ~value);
}

uint64_t LowestRun(uint64_t bits) {
	const uint64_t low = bits & (0 - bits);
	const uint64_t after = (bits + low) & ~bits;
	return after ? after - low : 0 - low;
}

bool InQuotes(CSVState state) {
	return state == CSVState::QUOTED || state == CSVState::QUOTED_NEW_LINE || state == CSVState::ESCAPE;
}

bool PartialDelimiter(CSVState state) {
	return state == CSVState::DELIMITER_FIRST_BYTE || state == CSVState::DELIMITER_SECOND_BYTE ||
	       state == CSVState::DELIMITER_THIRD_BYTE;
}

} // namespace

CsvGrammar CsvGrammar::For(const CSVStateMachine &state_machine) {
	CsvGrammar grammar;
	auto &dialect = state_machine.dialect_options.state_machine_options;
	const auto delimiter = dialect.delimiter.GetValue();
	grammar.transitions = &state_machine.transition_array;
	grammar.delimiter_size = MinValue<idx_t>(MaxValue<idx_t>(delimiter.size(), 1), 4);
	for (idx_t i = 0; i < grammar.delimiter_size && i < delimiter.size(); i++) {
		grammar.delimiter[i] = delimiter[i];
	}
	grammar.extra_delimiter_bytes = delimiter.empty() ? 0 : delimiter.size() - 1;
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

CsvIndexer::CsvIndexer(const CsvGrammar &grammar_p, bool detailed_p) : grammar(grammar_p), detailed(detailed_p) {
	Reset(CSVState::NOT_SET);
}

void CsvIndexer::Reset(CSVState state_p) {
	snapshot.pending = false;
	state = state_p;
	previous_state = state_p;
	before_block = state_p;
	CarryFromState(state_p);
}

void CsvIndexer::CarryFromState(CSVState current) {
	const auto &g = grammar;
	const bool in = InQuotes(current);
	carry = Carry();
	carry.in = in;
	carry.entry = in;
	carry.opened = in || current == CSVState::UNQUOTED || current == CSVState::MAYBE_QUOTED;
	carry.start = current == CSVState::NOT_SET || current == CSVState::DELIMITER ||
	              current == CSVState::RECORD_SEPARATOR || current == CSVState::CARRIAGE_RETURN;
	carry.empty_space = current == CSVState::EMPTY_SPACE;
	carry.escaped = current == CSVState::ESCAPE || current == CSVState::UNQUOTED_ESCAPE;
	carry.unquoted = current == CSVState::UNQUOTED;
	carry.carriage_return = g.carry_on_newline && current == CSVState::CARRIAGE_RETURN;
	carry.comment = current == CSVState::COMMENT;
	carry.partial = PartialDelimiter(current);
	if (g.delimiter_size > 1 && g.distinct_delimiter) {
		if (current == CSVState::DELIMITER_FIRST_BYTE) {
			carry.prefix[0] = uint64_t(1) << 63;
		} else if (current == CSVState::DELIMITER_SECOND_BYTE) {
			carry.prefix[1] = uint64_t(1) << 63;
		} else if (current == CSVState::DELIMITER_THIRD_BYTE) {
			carry.prefix[2] = uint64_t(1) << 63;
		}
	}
	carry.delimiter_run = !g.distinct_delimiter && current == CSVState::DELIMITER_FIRST_BYTE;
	const idx_t progress = current == CSVState::DELIMITER_FIRST_BYTE    ? 1
	                       : current == CSVState::DELIMITER_SECOND_BYTE ? 2
	                       : current == CSVState::DELIMITER_THIRD_BYTE  ? 3
	                                                                    : 0;
	carry.exact = current == CSVState::MAYBE_QUOTED || current == CSVState::ESCAPED_RETURN ||
	              current == CSVState::STANDARD_NEWLINE ||
	              (carry.partial && !g.distinct_delimiter && g.delimiter_size > 2) ||
	              (progress && progress >= g.delimiter_size) || (current == CSVState::ESCAPE && !g.quoted_escape) ||
	              (current == CSVState::UNQUOTED_ESCAPE && !g.unquoted_escape) ||
	              (current == CSVState::CARRIAGE_RETURN && !g.carry_on_newline) ||
	              (current == CSVState::EMPTY_SPACE && !g.empty_space);
}

void CsvIndexer::Match(const char *data, idx_t size, Masks &m) const {
	const auto &g = grammar;
	char padded[CSV_BLOCK_BYTES];
	const char *block = data;
	if (size < CSV_BLOCK_BYTES) {
		memset(padded, 0, sizeof(padded));
		memcpy(padded, data, size);
		block = padded;
	}
	const CsvVector vector(block);
	m.quote = g.quote ? vector.Equal(g.quote) : 0;
	m.escape = g.quoted_escape ? vector.Equal(g.escape) : 0;
	m.comment = g.comment ? vector.Equal(g.comment) : 0;
	m.line_feed = vector.Equal('\n');
	m.carriage_return = vector.Equal('\r');
	m.space = vector.Equal(' ');
	m.equal[0] = vector.Equal(g.delimiter[0]);
	m.delimiter_byte = m.equal[0];
	for (idx_t k = 1; k < 4; k++) {
		m.equal[k] = k < g.delimiter_size ? vector.Equal(g.delimiter[k]) : 0;
		m.delimiter_byte |= m.equal[k];
	}
}

void CsvIndexer::Delimiters(const Masks &m, uint64_t allowed, uint64_t region, idx_t last, Delimiter &d) const {
	const auto &g = grammar;
	const idx_t size = g.delimiter_size;
	d.partial = 0;
	d.progress[0] = d.progress[1] = d.progress[2] = 0;
	d.run = 0;
	d.phase = 0;
	if (size == 1) {
		d.ends = m.equal[0] & allowed & ~region;
		d.bytes = d.ends;
		return;
	}
	if (!g.distinct_delimiter) {
		if (size > 2) {
			d.ends = 0;
			d.bytes = 0;
			return;
		}
		const uint64_t run = m.equal[0] & allowed & ~region;
		const uint64_t starts = run & ~((run << 1) | carry.delimiter_run);
		uint64_t even_from = starts & EVEN_BITS;
		uint64_t odd_from = starts & ODD_BITS;
		if (carry.delimiter_run && (run & 1)) {
			if (carry.delimiter_phase) {
				even_from |= 1;
			} else {
				odd_from |= 1;
			}
		}
		const uint64_t even_region = Fill(even_from, odd_from);
		const uint64_t odd_offset = (even_region & ODD_BITS) | (~even_region & EVEN_BITS);
		d.ends = run & odd_offset;
		d.bytes = d.ends | (d.ends >> 1);
		d.partial = run & ~d.ends;
		d.progress[0] = d.partial;
		d.run = (run >> last) & 1;
		d.phase = (d.ends >> last) & 1;
		return;
	}
	uint64_t prefix = m.equal[0] & allowed & ~region;
	d.progress[0] = prefix;
	for (idx_t k = 1; k < size; k++) {
		const uint64_t previous = (prefix << 1) | (carry.prefix[k - 1] >> 63);
		prefix = m.equal[k] & previous;
		if (k + 1 < size) {
			d.progress[k] = prefix;
		}
	}
	d.ends = prefix;
	d.bytes = d.ends;
	for (idx_t k = 1; k < size; k++) {
		d.bytes |= d.ends >> k;
	}
	for (idx_t k = 0; k + 1 < size; k++) {
		d.partial |= d.progress[k];
	}
}

void CsvIndexer::Compute(const Masks &m, uint64_t region, idx_t last, Base &r) const {
	const auto &g = grammar;
	r.escaped = 0;
	r.escapes = 0;
	if (g.quoted_escape) {
		const uint64_t escape = m.escape & ~region;
		const uint64_t potential = escape & ~carry.escaped;
		const uint64_t series = ((potential << 1) | ODD_BITS) - potential;
		const uint64_t code = series ^ ODD_BITS;
		r.escaped = code ^ (escape | carry.escaped);
		r.escapes = code & escape;
	}
	const uint64_t unescaped = g.unquoted_escape ? ~r.escaped : ~uint64_t(0);
	r.quotes = m.quote & ~r.escaped & ~region;
	r.newline = (m.line_feed | m.carriage_return) & unescaped & ~region;
	uint64_t allowed = unescaped;
	if (g.carry_on_newline) {
		allowed &= ~(((m.carriage_return & unescaped) << 1) | carry.carriage_return);
	}
	Delimiters(m, allowed, region, last, r.delimiter);
	r.separator = r.delimiter.ends | r.newline;
	r.start = (r.separator << 1) | carry.start;
	uint64_t open_first = r.quotes & r.start;
	if (g.empty_space) {
		open_first |= r.quotes & (((r.start & m.space & ~r.separator & ~region) << 1) | carry.empty_space);
	}
	const uint64_t seed = carry.opened && !(r.start & 1) ? 1 : 0;
	r.opened = Fill(open_first | seed, r.start & ~open_first);
	r.parity = CsvPrefixXor(r.quotes) ^ CsvSpread(carry.parity);
	r.held = Hold(r.parity, r.separator, carry.hold);
	const uint64_t before = (r.held << 1) | carry.hold;
	const uint64_t segment = r.parity ^ before;
	const uint64_t toggles = r.separator & r.opened & segment;
	const uint64_t resets = r.separator & ~r.opened & segment;
	const uint64_t flips = CsvPrefixXor(toggles);
	r.entry_after = flips ^ Hold(flips, resets, carry.entry);
	r.entry = (r.entry_after << 1) | carry.entry;
	r.in = (r.entry ^ segment) & (r.opened | r.entry);
}

bool CsvIndexer::Bits(const Masks &m, uint64_t valid, idx_t last, CsvBlockBits &bits) {
	const auto &g = grammar;
	const uint64_t newline_bytes = m.line_feed | m.carriage_return;
	uint64_t region = 0;
	if (carry.comment) {
		const uint64_t first = newline_bytes & (0 - newline_bytes);
		region = first ? first - 1 : ~uint64_t(0);
	}
	Base base;
	while (true) {
		Compute(m, region, last, base);
		if (!g.comment) {
			break;
		}
		const uint64_t in_before = (base.in << 1) | carry.in;
		uint64_t candidates = m.comment & ~region & ~in_before & valid;
		if (g.unquoted_escape) {
			candidates &= ~base.escaped;
		}
		candidates &= ~((base.delimiter.partial << 1) | carry.partial);
		if (!candidates) {
			break;
		}
		const uint64_t fresh = Fill(candidates, newline_bytes) & ~region;
		if (!fresh) {
			break;
		}
		if (!(fresh & (m.quote | m.escape))) {
			region |= fresh;
			break;
		}
		region |= LowestRun(fresh);
	}

	const uint64_t in = base.in & ~region;
	const uint64_t in_before = (in << 1) | carry.in;
	const uint64_t separator = base.separator & ~region;
	const uint64_t partial = base.delimiter.partial & ~in & ~region;
	const uint64_t unquoted_after = (base.opened | base.entry) & ~in & ~separator & ~region & ~partial;
	const uint64_t unquoted_before = (unquoted_after << 1) | carry.unquoted;
	const uint64_t comment_start = region & ~((region << 1) | carry.comment);
	const uint64_t escapes_in = g.quoted_escape ? base.escapes & in : 0;
	uint64_t unquoted_escapes = 0;
	if (g.unquoted_escape) {
		unquoted_escapes = base.escapes & ~in & ~in_before & ~unquoted_before & ~region;
	}

	uint64_t anomaly = 0;
	if (g.strict_mode) {
		uint64_t allowed = separator | comment_start | base.delimiter.bytes;
		if (g.trim_quoted_spaces) {
			allowed |= m.space;
		}
		if (g.quote == g.escape) {
			allowed |= base.quotes;
		}
		anomaly |= unquoted_before & ~allowed;
		const uint64_t line_feed = m.line_feed & ~in & ~region;
		const uint64_t carriage_return = m.carriage_return & ~in & ~region;
		switch (g.new_line) {
		case NewLineIdentifier::CARRY_ON:
			anomaly |= line_feed & ~((m.carriage_return << 1) | carry.carriage_return);
			anomaly |= carriage_return & ~(m.line_feed >> 1) & ~(uint64_t(1) << last);
			if (carry.carriage_return && !(m.line_feed & 1)) {
				anomaly |= 1;
			}
			break;
		case NewLineIdentifier::SINGLE_N:
			anomaly |= carriage_return;
			break;
		case NewLineIdentifier::SINGLE_R:
			anomaly |= line_feed;
			break;
		default:
			break;
		}
	} else {
		if (g.quoted_escape) {
			anomaly |= unquoted_before & m.escape;
		}
		if (!g.escape && g.quote) {
			anomaly |= unquoted_before & m.quote;
		}
	}
	if (g.unquoted_escape && g.carry_on_newline) {
		anomaly |= m.carriage_return & base.escaped & ~in & ~region;
	}
	if (g.delimiter_size > 1) {
		const uint64_t after_partial = (partial << 1) | carry.partial;
		anomaly |= (m.comment | m.escape) & ~region & after_partial;
		anomaly |= unquoted_before & m.delimiter_byte & ~base.delimiter.bytes;
		if (!g.distinct_delimiter && g.delimiter_size > 2) {
			anomaly |= m.delimiter_byte & ~region;
		}
	}
	if (anomaly & valid) {
		return false;
	}

	bits.in = in & valid;
	bits.sep = base.delimiter.ends & ~in & ~region & valid;
	bits.term = base.newline & ~in & valid;
	bits.carriage = g.carry_on_newline ? bits.term & m.carriage_return : 0;
	bits.quoted_nl = newline_bytes & in & ~base.escaped & valid;
	bits.open = base.quotes & in & ~in_before & valid;
	bits.mark = escapes_in | unquoted_escapes;
	if (g.escape) {
		bits.mark |= unquoted_before & unquoted_after;
	}
	bits.mark &= valid;
	bits.unstrict = unquoted_escapes & valid;
	bits.ever = g.quoted_escape ? (base.escaped & in & (g.strict_mode ? m.quote : ~uint64_t(0))) & valid : 0;
	bits.comment = comment_start & valid;
	bits.invalid = 0;
	const uint64_t empty_spaces = g.empty_space ? base.start & m.space & ~separator & ~region & ~in : 0;
	if (detailed) {
		bits.commented = region & valid;
		bits.standard = valid & ~region & ~in & ~bits.sep & ~bits.term & ~unquoted_escapes & ~unquoted_after &
		                ~partial & ~empty_spaces;
	}

	snapshot.region = region;
	snapshot.in = in;
	snapshot.escapes = base.escapes;
	snapshot.quoted_nl = bits.quoted_nl;
	snapshot.sep = bits.sep;
	snapshot.term = bits.term;
	snapshot.carriage = bits.carriage;
	snapshot.unquoted_escapes = unquoted_escapes;
	snapshot.unquoted = unquoted_after;
	snapshot.partial = partial;
	snapshot.progress[0] = base.delimiter.progress[1];
	snapshot.progress[1] = base.delimiter.progress[2];
	snapshot.empty_spaces = empty_spaces;
	snapshot.last = last;
	snapshot.pending = true;

	const auto top = [last](uint64_t value) {
		return (value >> last) & 1;
	};
	carry.in = top(in);
	carry.start = top(separator);
	carry.opened = carry.start ? 0 : top((base.opened | base.entry) & ~region);
	carry.entry = carry.in;
	carry.parity = 0;
	carry.hold = 0;
	carry.empty_space = top(empty_spaces);
	carry.escaped = top(base.escapes);
	carry.unquoted = top(unquoted_after);
	carry.carriage_return = g.carry_on_newline ? top(m.carriage_return & ~base.escaped & ~in) : 0;
	carry.comment = top(region);
	carry.partial = top(partial);
	carry.delimiter_run = base.delimiter.run;
	carry.delimiter_phase = base.delimiter.phase;
	for (idx_t k = 0; k < 3; k++) {
		carry.prefix[k] = (base.delimiter.progress[k] & ~in & ~region) << (63 - last);
	}
	carry.exact = false;
	return true;
}

bool CsvIndexer::Fast(const Masks &m, uint64_t valid, idx_t last, CsvBlockBits &bits) {
	const auto &g = grammar;
	if (g.delimiter_size != 1 || carry.comment || carry.escaped || carry.partial || m.escape || m.comment) {
		return false;
	}
	const uint64_t quotes = m.quote;
	const uint64_t newline = m.line_feed | m.carriage_return;
	uint64_t delimiter = m.equal[0];
	if (g.carry_on_newline) {
		delimiter &= ~((m.carriage_return << 1) | carry.carriage_return);
	}
	const uint64_t separator = delimiter | newline;
	const uint64_t in = CsvPrefixXor(quotes) ^ CsvSpread(carry.in);
	const uint64_t in_before = (in << 1) | carry.in;
	const uint64_t start = (separator << 1) | carry.start;
	uint64_t open_first = quotes & start;
	if (g.empty_space) {
		open_first |= quotes & (((start & m.space & ~separator) << 1) | carry.empty_space);
	}
	const uint64_t opens = quotes & in & ~in_before;
	const uint64_t closes = quotes & ~in & in_before;
	const uint64_t after_close = (closes << 1) | carry.unquoted;
	if ((opens & ~open_first & ~after_close) || (after_close & ~(separator | quotes) & valid)) {
		return false;
	}
	if ((after_close & quotes & valid) && (!g.escape || (g.strict_mode && g.escape != g.quote))) {
		return false;
	}
	if (g.strict_mode) {
		const uint64_t line_feed = m.line_feed & ~in;
		const uint64_t carriage_return = m.carriage_return & ~in;
		switch (g.new_line) {
		case NewLineIdentifier::CARRY_ON:
			if ((line_feed & ~((m.carriage_return << 1) | carry.carriage_return) & valid) ||
			    (carriage_return & ~(m.line_feed >> 1) & ~(uint64_t(1) << last) & valid) ||
			    (carry.carriage_return && !(m.line_feed & 1))) {
				return false;
			}
			break;
		case NewLineIdentifier::SINGLE_N:
			if (carriage_return & valid) {
				return false;
			}
			break;
		case NewLineIdentifier::SINGLE_R:
			if (line_feed & valid) {
				return false;
			}
			break;
		default:
			break;
		}
	}
	const uint64_t empty_spaces = g.empty_space ? start & m.space & ~separator & ~in : 0;
	bits.in = in & valid;
	bits.sep = delimiter & ~in & valid;
	bits.term = newline & ~in & valid;
	bits.carriage = g.carry_on_newline ? bits.term & m.carriage_return : 0;
	bits.quoted_nl = newline & in & valid;
	bits.open = opens & valid;
	bits.mark = 0;
	bits.unstrict = 0;
	bits.ever = 0;
	bits.comment = 0;
	bits.invalid = 0;
	if (detailed) {
		bits.commented = 0;
		bits.standard = valid & ~in & ~bits.sep & ~bits.term & ~closes & ~empty_spaces;
	}

	snapshot.region = 0;
	snapshot.in = in;
	snapshot.escapes = 0;
	snapshot.quoted_nl = bits.quoted_nl;
	snapshot.sep = bits.sep;
	snapshot.term = bits.term;
	snapshot.carriage = bits.carriage;
	snapshot.unquoted_escapes = 0;
	snapshot.unquoted = closes;
	snapshot.partial = 0;
	snapshot.progress[0] = 0;
	snapshot.progress[1] = 0;
	snapshot.empty_spaces = empty_spaces;
	snapshot.last = last;
	snapshot.pending = true;

	const auto top = [last](uint64_t value) {
		return (value >> last) & 1;
	};
	carry.in = top(in);
	carry.start = top(separator);
	carry.unquoted = top(closes);
	carry.opened = carry.start ? 0 : carry.in | carry.unquoted;
	carry.entry = carry.in;
	carry.parity = 0;
	carry.hold = 0;
	carry.empty_space = top(empty_spaces);
	carry.carriage_return = g.carry_on_newline ? top(m.carriage_return & ~in) : 0;
	return true;
}

CSVState CsvIndexer::At(idx_t i) const {
	const auto &g = grammar;
	const auto &s = snapshot;
	const uint64_t bit = uint64_t(1) << i;
	if (s.region & bit) {
		return CSVState::COMMENT;
	}
	if (s.in & bit) {
		if (s.escapes & bit) {
			return CSVState::ESCAPE;
		}
		if (s.quoted_nl & bit) {
			return CSVState::QUOTED_NEW_LINE;
		}
		return CSVState::QUOTED;
	}
	if (s.sep & bit) {
		return CSVState::DELIMITER;
	}
	if (s.term & bit) {
		return s.carriage & bit ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR;
	}
	if (s.unquoted_escapes & bit) {
		return CSVState::UNQUOTED_ESCAPE;
	}
	if (s.unquoted & bit) {
		return CSVState::UNQUOTED;
	}
	if (s.partial & bit) {
		if (s.progress[1] & bit) {
			return CSVState::DELIMITER_THIRD_BYTE;
		}
		if (s.progress[0] & bit) {
			return CSVState::DELIMITER_SECOND_BYTE;
		}
		return CSVState::DELIMITER_FIRST_BYTE;
	}
	if (g.empty_space && (s.empty_spaces & bit)) {
		return CSVState::EMPTY_SPACE;
	}
	return CSVState::STANDARD;
}

CSVState CsvIndexer::State() const {
	return snapshot.pending ? At(snapshot.last) : state;
}

CSVState CsvIndexer::PreviousState() const {
	if (!snapshot.pending) {
		return previous_state;
	}
	return snapshot.last ? At(snapshot.last - 1) : before_block;
}

void CsvIndexer::Settle() {
	if (snapshot.pending) {
		previous_state = PreviousState();
		state = State();
		snapshot.pending = false;
	}
}

void CsvIndexer::Walk(const char *data, idx_t size, CsvBlockBits &bits) {
	const auto &g = grammar;
	const auto &table = g.transitions->state_machine;
	Settle();
	bits = CsvBlockBits();
	CSVState current = state;
	CSVState before = previous_state;
	for (idx_t i = 0; i < size; i++) {
		const auto byte = static_cast<uint8_t>(data[i]);
		const CSVState from = current;
		current = table[byte][static_cast<uint8_t>(from)];
		before = from;
		const uint64_t bit = uint64_t(1) << i;
		switch (current) {
		case CSVState::DELIMITER:
			bits.sep |= bit;
			break;
		case CSVState::RECORD_SEPARATOR:
			bits.term |= bit;
			break;
		case CSVState::CARRIAGE_RETURN:
			bits.term |= bit;
			bits.carriage |= bit;
			break;
		case CSVState::QUOTED:
			if (!InQuotes(from)) {
				bits.open |= bit;
			}
			if ((from == CSVState::ESCAPE || from == CSVState::ESCAPED_RETURN || from == CSVState::UNQUOTED_ESCAPE) &&
			    (byte == static_cast<uint8_t>(g.quote) || !g.strict_mode)) {
				bits.ever |= bit;
			}
			bits.in |= bit;
			break;
		case CSVState::QUOTED_NEW_LINE:
			bits.quoted_nl |= bit;
			bits.in |= bit;
			break;
		case CSVState::ESCAPE:
			bits.mark |= bit;
			bits.in |= bit;
			break;
		case CSVState::ESCAPED_RETURN:
			bits.mark |= bit;
			break;
		case CSVState::UNQUOTED_ESCAPE:
			bits.mark |= bit;
			bits.unstrict |= bit;
			break;
		case CSVState::UNQUOTED:
			if (from == CSVState::MAYBE_QUOTED || (from == CSVState::UNQUOTED && g.escape != '\0')) {
				bits.mark |= bit;
			}
			if (from == CSVState::MAYBE_QUOTED) {
				bits.ever |= bit;
			}
			break;
		case CSVState::COMMENT:
			if (from != CSVState::COMMENT) {
				bits.comment |= bit;
			}
			bits.commented |= bit;
			break;
		case CSVState::INVALID:
			bits.invalid |= bit;
			break;
		case CSVState::STANDARD:
			bits.standard |= bit;
			break;
		default:
			break;
		}
	}
	previous_state = before;
	state = current;
	CarryFromState(current);
}

void CsvIndexer::Index(const char *data, idx_t size, CsvBlockBits &bits) {
	D_ASSERT(size > 0 && size <= CSV_BLOCK_BYTES);
	Masks masks;
	Match(data, size, masks);
	const uint64_t valid = size == CSV_BLOCK_BYTES ? ~uint64_t(0) : (uint64_t(1) << size) - 1;
	if (size == 1) {
		before_block = State();
	}
	if (!carry.exact && (Fast(masks, valid, size - 1, bits) || Bits(masks, valid, size - 1, bits))) {
		return;
	}
	Walk(data, size, bits);
}

} // namespace duckdb
