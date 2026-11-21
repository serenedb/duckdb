#include "duckdb/formats/row_ranges.hpp"

#include "absl/cleanup/cleanup.h"
#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/formats/csv_simd.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "simdutf.h"

namespace duckdb {

namespace {

constexpr idx_t RANGE_OVERLAP = 1 << 12;
constexpr idx_t SYNC_WINDOW = 1 << 14;
constexpr idx_t WINDOW_BYTES = 1 << 18;
constexpr idx_t UNSETTLED_BYTES = 1 << 22;
constexpr idx_t FEED_BYTES = 1 << 20;
constexpr idx_t REBASE_BYTES = 1 << 29;
constexpr idx_t ROW_LIMIT = idx_t(3) << 29;

char LastDelimiterByte(const RowDialect &dialect) {
	return dialect.delimiter_size > 1 ? dialect.delimiter_tail[dialect.delimiter_size - 2] : dialect.delimiter;
}

char *OwnedData(RowBytes &bytes) {
	return char_ptr_cast(bytes.owned.get());
}

bool QuotedEscape(const RowDialect &dialect) {
	return dialect.escape != '\0' && dialect.escape != dialect.quote;
}

bool OddCount(std::string_view bytes, char byte) {
	uint64_t parity = 0;
	idx_t at = 0;
	for (; at + CSV_BLOCK_BYTES <= bytes.size(); at += CSV_BLOCK_BYTES) {
		parity ^= CsvVector(bytes.data() + at).Equal(byte);
	}
	bool odd = std::popcount(parity) % 2 != 0;
	for (; at < bytes.size(); at++) {
		odd = odd != (bytes[at] == byte);
	}
	return odd;
}

bool Contains(std::string_view bytes, char byte) {
	return byte != '\0' && bytes.find(byte) != std::string_view::npos;
}

idx_t FirstNewline(const RowDialect &dialect, const char *bytes, idx_t base, idx_t begin, idx_t end, bool eof) {
	const auto at = [&](idx_t position) {
		return bytes[position - base];
	};
	const bool escapes = QuotedEscape(dialect);
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
		uint64_t candidates =
		    dialect.carriage_terms ? vector.Equal('\n') | vector.Equal('\r') : vector.Equal(dialect.line_break);
		if (size < CSV_BLOCK_BYTES) {
			candidates &= (uint64_t(1) << size) - 1;
		}
		while (candidates) {
			const idx_t position = block + CountZeros<uint64_t>::Trailing(candidates);
			candidates &= candidates - 1;
			if (dialect.carriage_terms && at(position) == '\r') {
				if (position + 1 >= end) {
					if (!eof) {
						return DConstants::INVALID_INDEX;
					}
				} else if (at(position + 1) == '\n') {
					continue;
				}
			}
			if (escapes) {
				if (position > base && at(position - 1) == dialect.escape) {
					continue;
				}
				if (position > base + 1 && at(position - 1) == '\r' && at(position - 2) == dialect.escape) {
					continue;
				}
			}
			return position + 1;
		}
	}
	return DConstants::INVALID_INDEX;
}

bool OpensRecord(const RowDialect &dialect, const char *bytes, idx_t base, idx_t candidate, idx_t limit) {
	const auto at = [&](idx_t position) {
		return bytes[position - base];
	};
	const auto found = static_cast<const char *>(memchr(bytes + (candidate - base), dialect.quote, limit - candidate));
	if (!found) {
		return false;
	}
	const idx_t quote = static_cast<idx_t>(found - bytes) + base;
	if (quote + 1 >= limit) {
		return false;
	}
	const char before = quote == candidate ? '\n' : at(quote - 1);
	const char after = at(quote + 1);
	const bool field_start = before == LastDelimiterByte(dialect) || before == '\n' || before == '\r';
	const bool data_follows = after != dialect.delimiter && after != '\n' && after != '\r' && after != dialect.quote &&
	                          after != dialect.escape;
	return field_start && data_follows;
}

} // namespace

idx_t RowSource::ReadFully(char *data, idx_t count, idx_t offset) const {
	idx_t total = Read(data, count, offset);
	while (total < count && Live()) {
		const idx_t more = Read(data + total, count - total, offset + total);
		if (more == 0) {
			break;
		}
		total += more;
	}
	return total;
}

idx_t RowSync(const RowDialect &dialect, const char *bytes, idx_t base, idx_t begin, idx_t end, bool eof) {
	idx_t first = FirstNewline(dialect, bytes, base, begin, end, eof);
	if (first != DConstants::INVALID_INDEX && dialect.backslash) {
		const bool pairs = dialect.line_break == '\n';
		while (first < end) {
			const char byte = bytes[first - base];
			if (byte == dialect.line_break) {
				first++;
			} else if (pairs && byte == '\r' && first + 1 < end && bytes[first + 1 - base] == '\n') {
				first += 2;
			} else if (pairs && byte == '\r' && first + 1 == end && !eof) {
				return DConstants::INVALID_INDEX;
			} else {
				break;
			}
		}
		return first == end && !eof ? DConstants::INVALID_INDEX : first;
	}
	if (first == DConstants::INVALID_INDEX || !dialect.quote) {
		return first;
	}
	const idx_t window = first + SYNC_WINDOW;
	if (window > end && !eof) {
		return DConstants::INVALID_INDEX;
	}
	const idx_t limit = MinValue(window, end);
	for (idx_t candidate = first; candidate < limit;) {
		if (OpensRecord(dialect, bytes, base, candidate, limit)) {
			return candidate;
		}
		const idx_t next = FirstNewline(dialect, bytes, base, candidate, limit, true);
		if (next == DConstants::INVALID_INDEX || next >= limit) {
			break;
		}
		candidate = next;
	}
	return first;
}

RowRanges::RowRanges(Allocator &allocator_p, const RowLexer &lexer_p, RowSource &source_p, idx_t start_p, idx_t size_p,
                     idx_t range_bytes_p, bool file_start_p)
    : allocator(allocator_p), lexer(&lexer_p), source(source_p), start(start_p),
      stream(size_p == NumericLimits<idx_t>::Maximum()), file_start(file_start_p), size(size_p) {
	Layout(range_bytes_p);
}

RowRanges::RowRanges(Allocator &allocator_p, RowSource &source_p, idx_t start_p, idx_t size_p, idx_t range_bytes_p)
    : allocator(allocator_p), source(source_p), start(start_p), stream(size_p == NumericLimits<idx_t>::Maximum()),
      file_start(true), size(size_p) {
	Layout(range_bytes_p);
}

void RowRanges::Reset(idx_t size_p, idx_t range_bytes_p) {
	D_ASSERT(!stream && size_p != NumericLimits<idx_t>::Maximum());
	const absl::MutexLock guard(&lock);
	size.store(size_p, std::memory_order_release);
	next_range = 0;
	finished_ranges = 0;
	Layout(range_bytes_p);
}

void RowRanges::Layout(idx_t range_bytes_p) {
	const idx_t end = size.load(std::memory_order_relaxed);
	intake = stream && range_bytes_p != DConstants::INVALID_INDEX;
	idx_t count;
	if (intake) {
		range_bytes = MaxValue<idx_t>(range_bytes_p, 1);
		count = NumericLimits<idx_t>::Maximum();
	} else if (stream || range_bytes_p == DConstants::INVALID_INDEX) {
		range_bytes = end - MinValue(start, end);
		count = 1;
	} else {
		range_bytes = MaxValue<idx_t>(range_bytes_p, 1);
		const idx_t data_size = end - MinValue(start, end);
		count = MaxValue<idx_t>((data_size + range_bytes - 1) / range_bytes, 1);
	}
	range_count.store(count, std::memory_order_release);
	CsvRangeStart first;
	first.line_end = start;
	first.last_position = start;
	chain.Assign(intake ? 1 : count, first);
	intake_begin = start;
	intake_carry.reset();
	intake_carry_to = start;
	intake_origin = CSVState::NOT_SET;
	intake_line_end = start;
	serving = 0;
	failed = false;
}

void RowRanges::FoundEnd(idx_t end) {
	const absl::MutexLock guard(&lock);
	if (end < size.load(std::memory_order_relaxed)) {
		size.store(end, std::memory_order_release);
	}
}

void RowRanges::SetWholeFile(shared_ptr<RowBuffer> buffer, const char *data) {
	whole_buffer = std::move(buffer);
	whole_data = data;
}

void RowRanges::ReadInto(RowBytes &bytes, idx_t offset, idx_t count) {
	const idx_t end = Size();
	count = offset < end ? MinValue(count, end - offset) : 0;
	if (whole_data) {
		bytes.buffer = whole_buffer;
		bytes.data = whole_data + offset;
		bytes.offset = offset;
		bytes.size = count;
		return;
	}
	if (bytes.capacity < count || !bytes.owned.IsSet()) {
		bytes.capacity = MaxValue<idx_t>(count, 1);
		bytes.owned = allocator.Allocate(bytes.capacity);
	}
	bytes.buffer.reset();
	bytes.data = OwnedData(bytes);
	bytes.offset = offset;
	bytes.size = count ? Read(OwnedData(bytes), count, offset) : 0;
	if (stream && bytes.size < count) {
		const auto known = source.End();
		if (known.IsValid()) {
			FoundEnd(known.GetIndex());
		} else if (bytes.size > 0 && !source.Live()) {
			FoundEnd(offset + bytes.size);
		}
	}
}

void RowRanges::ViewWholeFile(RowBytes &bytes) const {
	bytes.owned.Reset();
	bytes.capacity = 0;
	bytes.buffer = whole_buffer;
	bytes.data = whole_data;
	bytes.offset = 0;
	bytes.size = Size();
}

RowRangeBounds RowRanges::Bounds(idx_t range, RowBytes &bytes) {
	const idx_t begin = RangeBegin(range);
	const idx_t end = RangeEnd(range);
	const idx_t from = begin >= 3 ? begin - 3 : 0;
	idx_t want = end - from + RANGE_OVERLAP;
	ReadInto(bytes, from, want);
	const auto sync = [&](idx_t position) {
		while (true) {
			const idx_t available = bytes.offset + bytes.size;
			const bool eof = available >= Size();
			const idx_t found = RowSync(lexer->Dialect(), bytes.data, bytes.offset, position, available, eof);
			if (found != DConstants::INVALID_INDEX) {
				return found;
			}
			if (eof) {
				return Size();
			}
			want *= 2;
			ReadInto(bytes, from, want);
		}
	};
	RowRangeBounds bounds;
	bounds.begin = range == 0 ? start : sync(begin - 1);
	bounds.end = range + 1 == RangeCount() ? Size() : sync(end - 1);
	bounds.end = MaxValue(bounds.end, bounds.begin);
	return bounds;
}

CSVState RowRanges::Origin(const RowBytes &bytes, idx_t begin) const {
	if (begin == start) {
		return CSVState::NOT_SET;
	}
	const char newline = bytes.data[begin - 1 - bytes.offset];
	return newline == '\r' && lexer->Dialect().carry_on ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR;
}

CsvRangeEntry RowRanges::Transfer(idx_t range, CSVState origin) {
	RowBytes bytes;
	const auto bounds = Bounds(range, bytes);
	return LexTransfer(bytes, bounds.begin, bounds.end, origin);
}

CsvRangeEntry RowRanges::LexTransfer(const RowBytes &bytes, idx_t begin, idx_t end, CSVState origin) const {
	RowCursor cursor;
	lexer->Start(cursor, begin, true, false);
	switch (origin) {
	case CSVState::QUOTED:
	case CSVState::QUOTED_NEW_LINE:
		cursor.in_quotes = 1;
		break;
	case CSVState::STANDARD:
		cursor.after_term = 0;
		break;
	case CSVState::CARRIAGE_RETURN:
		cursor.after_cr = 1;
		break;
	default:
		cursor.file_start = begin == start && file_start;
		break;
	}
	lexer->Feed(cursor, bytes.data, bytes.offset, end, true, nullptr);
	CsvRangeEntry entry;
	lexer->Finish(cursor, bytes.data, bytes.offset, end, end >= Size(), nullptr, origin, entry);
	return entry;
}

idx_t RowRanges::SyncAt(idx_t position) {
	const idx_t from = position >= 2 ? position - 2 : 0;
	idx_t want = RANGE_OVERLAP;
	RowBytes bytes;
	while (true) {
		ReadInto(bytes, from, want);
		const idx_t available = bytes.offset + bytes.size;
		const bool eof = available >= Size();
		const idx_t found = RowSync(lexer->Dialect(), bytes.data, bytes.offset, position, available, eof);
		if (found != DConstants::INVALID_INDEX) {
			return found;
		}
		if (eof) {
			return Size();
		}
		want *= 2;
	}
}

bool RowRanges::Claim(idx_t &range) {
	const absl::MutexLock guard(&lock);
	const bool left = intake ? !failed && intake_begin < size.load(std::memory_order_relaxed)
	                         : next_range < range_count.load(std::memory_order_relaxed);
	if (!left) {
		return false;
	}
	range = next_range++;
	return true;
}

bool RowRanges::ClaimIntake(idx_t range, RowClaim &claim) {
	const auto turn = [&]() {
		return failed || serving == range;
	};
	const auto take = [&]() {
		if (failed) {
			return false;
		}
		if (intake_begin >= size.load(std::memory_order_relaxed)) {
			serving++;
			finished_ranges++;
			return false;
		}
		claim.begin = intake_begin;
		claim.carry = std::move(intake_carry);
		claim.carry_to = intake_carry_to;
		claim.origin = intake_origin;
		claim.line_end = intake_line_end;
		return true;
	};
	{
		const absl::MutexLock guard(&lock);
		if (turn()) {
			return take();
		}
	}
	TaskScheduler::BlockingScope blocking;
	const absl::MutexLock guard(&lock);
	lock.Await(absl::Condition(&turn));
	return take();
}

void RowRanges::Cut(idx_t end, shared_ptr<RowBytes> rest, idx_t read_to, CSVState origin, idx_t line_end) {
	const absl::MutexLock guard(&lock);
	intake_begin = end;
	intake_carry = read_to > end ? std::move(rest) : nullptr;
	intake_carry_to = MaxValue(end, read_to);
	intake_origin = origin;
	intake_line_end = line_end;
	source.Release(intake_carry_to);
	serving++;
}

void RowRanges::FailIntake() {
	const absl::MutexLock guard(&lock);
	failed = true;
}

bool RowRanges::Complete() const {
	if (intake) {
		return serving == next_range && intake_begin >= size.load(std::memory_order_relaxed) &&
		       finished_ranges == next_range;
	}
	return finished_ranges == MaxValue(range_count.load(std::memory_order_relaxed), next_range);
}

bool RowRanges::Finish(idx_t range) {
	bool complete;
	bool drained;
	{
		const absl::MutexLock guard(&lock);
		D_ASSERT(range < next_range);
		finished_ranges++;
		complete = Complete();
		drained = intake && !complete && serving == finished_ranges;
	}
	if (drained) {
		source.Nudge();
	}
	return complete;
}

bool RowRanges::AllFinished() const {
	const absl::MutexLock guard(&lock);
	return Complete();
}

bool RowRanges::NothingInFlight() const {
	const absl::MutexLock guard(&lock);
	return serving == finished_ranges;
}

void RowRanges::Computing(idx_t range, bool value) {
	const absl::MutexLock guard(&lock);
	if (value) {
		chain.computing[range]++;
		return;
	}
	D_ASSERT(chain.computing[range] > 0);
	chain.computing[range]--;
}

void RowRanges::Publish(idx_t range, CSVState origin, const CsvRangeEntry &entry) {
	const absl::MutexLock guard(&lock);
	chain.Publish(range, static_cast<uint8_t>(origin), entry, range_count.load(std::memory_order_relaxed));
}

CsvRangeEntry RowRanges::Entry(idx_t range, CSVState origin) const {
	const absl::MutexLock guard(&lock);
	return chain.transfers[range][static_cast<uint8_t>(origin)];
}

CsvRangeStart RowRanges::StartState(idx_t range) {
	return chain.StartState(
	    lock, range, [&](idx_t missing, uint8_t origin) { return Transfer(missing, static_cast<CSVState>(origin)); },
	    [&](idx_t missing, uint8_t origin, const CsvRangeEntry &entry) {
		    chain.Publish(missing, origin, entry, range_count.load(std::memory_order_relaxed));
	    });
}

RowWindow::RowWindow(RowRanges &ranges_p) : ranges(ranges_p) {
}

void RowWindow::Start(idx_t from, bool release_p) {
	release = release_p;
	if (bytes && bytes.use_count() > 1) {
		std::swap(bytes, spare);
	}
	if (!bytes || bytes.use_count() > 1) {
		bytes = make_shared_ptr<RowBytes>();
	}
	if (ranges.WholeFile()) {
		ranges.ViewWholeFile(*bytes);
		read_to = ranges.Size();
		return;
	}
	if (bytes->buffer) {
		bytes = make_shared_ptr<RowBytes>();
	}
	bytes->offset = from;
	bytes->size = 0;
	bytes->data = OwnedData(*bytes);
	read_to = from;
}

void RowWindow::Prepare(idx_t keep_from, idx_t need) {
	auto &window = *bytes;
	const idx_t kept = read_to - keep_from;
	const bool owned = window.owned.IsSet();
	const bool fits = owned && read_to - window.offset + need <= window.capacity;
	if (bytes.use_count() == 1 && owned && window.capacity >= kept + need) {
		if (keep_from > window.offset && (kept <= PIECE || !fits)) {
			memmove(OwnedData(window), OwnedData(window) + (keep_from - window.offset), kept);
			window.offset = keep_from;
			window.size = kept;
		}
		window.data = OwnedData(window);
		return;
	}
	if (fits) {
		return;
	}
	shared_ptr<RowBytes> next;
	if (spare && spare.use_count() == 1) {
		next = std::move(spare);
	} else {
		next = make_shared_ptr<RowBytes>();
	}
	if (!next->owned.IsSet() || next->capacity < kept + need) {
		const idx_t capacity = MaxValue(WINDOW_BYTES, kept * 2 + need);
		next->owned.Reset();
		next->owned = ranges.allocator.Allocate(capacity + RowWindow::PADDING);
		next->capacity = capacity;
	}
	if (kept) {
		memcpy(OwnedData(*next), window.data + (keep_from - window.offset), kept);
	}
	next->buffer.reset();
	next->data = OwnedData(*next);
	next->offset = keep_from;
	next->size = kept;
	if (bytes.use_count() > 1) {
		spare = std::move(bytes);
	}
	bytes = std::move(next);
}

void RowWindow::Seed(const RowBytes &from, idx_t end) {
	const idx_t count = end - read_to;
	Prepare(bytes->offset, count);
	auto &window = *bytes;
	memcpy(OwnedData(window) + (read_to - window.offset), from.data + (read_to - from.offset), count);
	window.size += count;
	read_to += count;
	memset(OwnedData(window) + (read_to - window.offset), 0, PADDING);
}

void RowWindow::Read(idx_t keep_from, idx_t piece) {
	const idx_t end = ranges.Size();
	const idx_t want = end > read_to ? MinValue<idx_t>(piece, end - read_to) : 0;
	Prepare(keep_from, want);
	auto &window = *bytes;
	const idx_t got = want ? ranges.Read(OwnedData(window) + (read_to - window.offset), want, read_to) : 0;
	window.size += got;
	read_to += got;
	memset(OwnedData(window) + (read_to - window.offset), 0, PADDING);
	if (!ranges.stream) {
		return;
	}
	if (got < want) {
		const auto known = ranges.source.End();
		if (known.IsValid()) {
			ranges.FoundEnd(known.GetIndex());
		} else if (!ranges.source.Live()) {
			ranges.FoundEnd(read_to);
		}
	}
	if (release && !ranges.Intake()) {
		ranges.source.Release(read_to);
	}
}

bool RowWindow::Waits() const {
	return ranges.stream && ranges.source.Live() && !ranges.source.Available(read_to);
}

RowLookup::RowLookup(RowRanges &ranges_p) : bytes(make_shared_ptr<RowBytes>()), ranges(ranges_p) {
}

void RowLookup::Read(idx_t from, idx_t want) {
	auto &arena = *bytes;
	if (arena.size + want > arena.capacity) {
		const idx_t capacity = MaxValue(arena.size + want, arena.capacity * 2);
		auto grown = ranges.allocator.Allocate(capacity + RowWindow::PADDING);
		if (arena.size) {
			memcpy(grown.get(), arena.owned.get(), arena.size);
		}
		arena.owned = std::move(grown);
		arena.capacity = capacity;
		arena.data = OwnedData(arena);
	}
	const idx_t got = ranges.Read(OwnedData(arena) + arena.size, want, from);
	arena.size += got;
	memset(OwnedData(arena) + arena.size, 0, RowWindow::PADDING);
	window_end = from + got;
	window_eof = got < want;
}

optional_idx RowLookup::Find(idx_t from, idx_t offset, idx_t want) {
	if (from < window_begin || offset >= window_end) {
		if (window_end > window_begin && !window_eof && from >= window_end && from - window_end < want) {
			Read(window_end, offset + want - window_end);
		} else {
			window_offset = bytes->size;
			window_begin = from;
			Read(from, offset + want - from);
		}
		if (offset >= window_end) {
			return optional_idx();
		}
	}
	return window_offset + (offset - window_begin);
}

void RowLookup::Extend(idx_t want) {
	Read(window_end, want);
}

RowRangeReader::RowRangeReader(RowRanges &ranges_p)
    : ranges(ranges_p), lexer(*ranges_p.lexer), dialect(ranges_p.lexer->Dialect()), columns(ranges_p.lexer->Columns()),
      window(ranges_p) {
}

void RowRangeReader::Start(idx_t range_p, bool extract_p, bool release_p, bool validate_count_utf8_p) {
	range = range_p;
	intake = false;
	extract_rows = extract_p;
	release = release_p;
	validate_count_utf8 = validate_count_utf8_p;
	force_extract = false;
	recover_pending = false;
	StartPass(false);
}

bool RowRangeReader::StartIntake(idx_t range_p, bool extract_p, bool release_p, bool validate_count_utf8_p) {
	RowClaim claim;
	if (!ranges.ClaimIntake(range_p, claim)) {
		return false;
	}
	bool cut = false;
	absl::Cleanup abandon = [&] {
		if (!cut) {
			ranges.FailIntake();
		}
	};
	range = range_p;
	intake = true;
	extract_rows = extract_p;
	release = release_p;
	validate_count_utf8 = validate_count_utf8_p;
	force_extract = false;
	recover_pending = false;
	lex_start = claim.begin;
	origin = claim.origin;
	line_end = claim.line_end;
	window.Start(lex_start, false);
	if (claim.carry && claim.carry_to > lex_start) {
		window.Seed(*claim.carry, claim.carry_to);
	}
	claim.carry.reset();
	lex_stop = CutIntake();
	const auto &bytes = *window.bytes;
	idx_t next_line_end = lex_stop;
	while (next_line_end > bytes.offset && (bytes.data[next_line_end - 1 - bytes.offset] == '\n' ||
	                                        bytes.data[next_line_end - 1 - bytes.offset] == '\r')) {
		next_line_end--;
	}
	const CSVState next_origin = lex_stop == lex_start ? origin : ranges.Origin(bytes, lex_stop);
	ranges.Cut(lex_stop, window.bytes, window.read_to, next_origin,
	           next_line_end > bytes.offset ? next_line_end : DConstants::INVALID_INDEX);
	cut = true;
	StartPass(true);
	return true;
}

idx_t RowRangeReader::CutIntake() {
	RowCursor probe;
	lexer.Start(probe, lex_start, true, false);
	probe.file_start = origin == CSVState::NOT_SET && ranges.file_start;
	probe.after_cr = origin == CSVState::CARRIAGE_RETURN;
	const bool quote_parity =
	    dialect.quoting == RowQuoting::ANYWHERE && (!dialect.escape || dialect.escape == dialect.quote);
	const bool strict_lines = dialect.duckdb && dialect.strict && dialect.new_line != NewLineIdentifier::NOT_SET;
	bool plain =
	    !quote_parity && !dialect.backslash && (!strict_lines || dialect.new_line == NewLineIdentifier::SINGLE_N);
	bool quoted = false;
	idx_t scanned_to = lex_start;
	idx_t want = ranges.range_bytes;
	while (true) {
		const idx_t read_to = window.read_to;
		if (read_to >= ranges.Size()) {
			return ranges.Size();
		}
		const auto &bytes = *window.bytes;
		const std::string_view scan(bytes.data + (scanned_to - bytes.offset), read_to - scanned_to);
		if (quote_parity) {
			quoted = quoted != OddCount(scan, dialect.quote);
		} else if (plain) {
			plain = !Contains(scan, dialect.quote) && !(dialect.unquoted_escape && Contains(scan, dialect.escape));
		}
		scanned_to = read_to;
		const bool lexed = !quote_parity && !dialect.backslash && !plain;
		if (lexed && read_to - probe.pos > CSV_BLOCK_BYTES + lexer.Lookahead()) {
			[[maybe_unused]] const bool fed = lexer.Feed(probe, bytes.data, bytes.offset, read_to - 1, false, nullptr);
			D_ASSERT(fed);
		}
		const bool idle = read_to - lex_start < want;
		if (idle && (read_to == lex_start || !window.Waits() || !ranges.NothingInFlight())) {
			window.Read(lex_start, want - (read_to - lex_start));
			continue;
		}
		RowCursor last = probe;
		if (quote_parity) {
			QuoteParityTerm(bytes, read_to, quoted, last);
		} else if (dialect.backslash) {
			EscapeParityTerm(bytes, idle ? read_to : LineBreaksFrom(bytes, read_to), last);
		} else if (plain) {
			PlainTerm(bytes, idle ? read_to : read_to - 1, last);
		} else if (idle && last.pos < read_to) {
			[[maybe_unused]] const bool fed = lexer.Feed(last, bytes.data, bytes.offset, read_to, true, nullptr);
			D_ASSERT(fed);
		}
		const idx_t cut = IntakeCut(last, idle);
		if (cut != DConstants::INVALID_INDEX) {
			return cut;
		}
		if (idle) {
			window.Read(lex_start, want - (read_to - lex_start));
			continue;
		}
		if (read_to - lex_start >= ROW_LIMIT) {
			throw InvalidInputException(
			    "The row at byte %llu of \"%s\" is longer than %llu bytes, the longest row that can be read",
			    probe.last_term == DConstants::INVALID_INDEX ? lex_start : probe.last_term + 1, ranges.path, ROW_LIMIT);
		}
		want = read_to - lex_start + RowWindow::PIECE;
	}
}

void RowRangeReader::QuoteParityTerm(const RowBytes &bytes, idx_t read_to, bool quoted, RowCursor &last) const {
	bool inside = quoted;
	for (idx_t at = read_to; at > lex_start; at--) {
		const char byte = bytes.data[at - 1 - bytes.offset];
		if (byte == dialect.quote) {
			inside = !inside;
		} else if (byte == dialect.line_break && !inside) {
			last.last_term = at - 1;
			last.last_term_cr = byte == '\r';
			return;
		}
	}
}

idx_t RowRangeReader::LineBreaksFrom(const RowBytes &bytes, idx_t read_to) const {
	idx_t from = read_to;
	while (from > lex_start &&
	       (bytes.data[from - 1 - bytes.offset] == '\n' || bytes.data[from - 1 - bytes.offset] == '\r')) {
		from--;
	}
	return from;
}

void RowRangeReader::EscapeParityTerm(const RowBytes &bytes, idx_t limit, RowCursor &last) const {
	const auto at = [&](idx_t position) {
		return bytes.data[position - bytes.offset];
	};
	for (idx_t term = limit; term > lex_start; term--) {
		if (at(term - 1) != dialect.line_break) {
			continue;
		}
		idx_t run = term - 1;
		while (run > lex_start && at(run - 1) == dialect.escape) {
			run--;
		}
		if ((term - 1 - run) % 2 == 0) {
			last.last_term = term - 1;
			last.last_term_cr = dialect.line_break == '\r';
			return;
		}
	}
}

void RowRangeReader::PlainTerm(const RowBytes &bytes, idx_t limit, RowCursor &last) const {
	const std::string_view piece(bytes.data + (lex_start - bytes.offset), limit - lex_start);
	idx_t found;
	if (!dialect.carriage_terms) {
		found = piece.rfind(dialect.line_break);
	} else if (dialect.strict && dialect.new_line == NewLineIdentifier::SINGLE_N) {
		found = piece.rfind('\n');
	} else {
		found = piece.find_last_of("\r\n");
	}
	if (found == std::string_view::npos) {
		return;
	}
	last.last_term = lex_start + found;
	last.last_term_cr = piece[found] == '\r';
}

idx_t RowRangeReader::IntakeCut(const RowCursor &probe, bool idle) const {
	if (probe.last_term == DConstants::INVALID_INDEX) {
		return DConstants::INVALID_INDEX;
	}
	const auto &bytes = *window.bytes;
	const idx_t read_to = window.read_to;
	const auto at = [&](idx_t position) {
		return bytes.data[position - bytes.offset];
	};
	const idx_t undecided = idle ? read_to : DConstants::INVALID_INDEX;
	idx_t cut = probe.last_term + 1;
	if (dialect.carry_on && probe.last_term_cr) {
		if (cut == read_to) {
			return undecided;
		}
		if (at(cut) == '\n') {
			cut++;
		}
	}
	if (!dialect.backslash) {
		return cut;
	}
	const bool pairs = dialect.line_break == '\n';
	while (cut < read_to) {
		const char byte = at(cut);
		if (byte == dialect.line_break) {
			cut++;
		} else if (pairs && byte == '\r' && cut + 1 < read_to && at(cut + 1) == '\n') {
			cut += 2;
		} else if (pairs && byte == '\r' && cut + 1 == read_to) {
			return idle ? cut : DConstants::INVALID_INDEX;
		} else {
			return cut;
		}
	}
	return undecided;
}

void RowRangeReader::Adopt(shared_ptr<RowBytes> bytes_p, const RowCursor &cursor_p) {
	window.bytes = std::move(bytes_p);
	cursor = cursor_p;
	active = false;
	finished = true;
	held_rows = 0;
	emitted_rows = 0;
	last_emitted_end = DConstants::INVALID_INDEX;
	line_end = DConstants::INVALID_INDEX;
	lex_start = 0;
}

void RowRangeReader::StartPass(bool second_pass_p) {
	active = true;
	extract = force_extract || extract_rows;
	probe_only = false;
	finished = false;
	extending = false;
	open_end = false;
	second_pass = second_pass_p;
	held_rows = 0;
	counted_rows = DConstants::INVALID_INDEX;
	range_lines = DConstants::INVALID_INDEX;
	emitted_rows = 0;
	last_emitted_end = DConstants::INVALID_INDEX;
	fields.Clear();
	if (!second_pass) {
		lex_start = DConstants::INVALID_INDEX;
		lex_stop = range + 1 == ranges.RangeCount() ? ranges.Size() : DConstants::INVALID_INDEX;
		line_end = range == 0 ? ranges.start : DConstants::INVALID_INDEX;
	}
	if (!intake) {
		const idx_t begin = ranges.RangeBegin(range);
		const idx_t from = second_pass ? lex_start : (range == 0 ? ranges.start : begin - MinValue<idx_t>(begin, 3));
		window.Start(from, release);
	}
	if (second_pass) {
		lexer.Start(cursor, lex_start, true);
		cursor.file_start = origin == CSVState::NOT_SET && ranges.file_start;
		cursor.after_cr = origin == CSVState::CARRIAGE_RETURN;
		validated_to = lex_start;
	}
}

void RowRangeReader::PublishOwnTransfer() {
	if (range + 1 >= ranges.RangeCount()) {
		return;
	}
	ranges.Computing(range, true);
	absl::Cleanup stop = [&] {
		ranges.Computing(range, false);
	};
	RowBytes span;
	const auto bounds = ranges.Bounds(range, span);
	if (dialect.quote) {
		const auto quoted = ranges.LexTransfer(span, bounds.begin, bounds.end, CSVState::QUOTED);
		ranges.Publish(range, CSVState::QUOTED, quoted);
		ranges.Publish(range, CSVState::QUOTED_NEW_LINE, quoted);
	}
	ranges.Publish(range, origin, ranges.LexTransfer(span, bounds.begin, bounds.end, origin));
}

bool RowRangeReader::FromChain() {
	PublishOwnTransfer();
	const auto start = ranges.StartState(range);
	const auto state = start.states[1];
	if (state == origin) {
		line_end = start.line_end;
		StartPass(true);
		return true;
	}
	auto entry = ranges.Entry(range, state);
	if (!entry.computed) {
		entry = ranges.Transfer(range, state);
		ranges.Publish(range, state, entry);
	}
	if (lex_stop == DConstants::INVALID_INDEX) {
		lex_stop = MaxValue(ranges.SyncAt(ranges.RangeEnd(range) - 1), lex_start);
	}
	if (entry.first_boundary == DConstants::INVALID_INDEX || entry.first_boundary >= lex_stop) {
		return false;
	}
	auto boundary = entry.boundary;
	boundary.Inherit(start);
	lex_start = entry.first_boundary;
	origin = boundary.states[1];
	line_end = boundary.line_end;
	StartPass(true);
	return true;
}

idx_t RowRangeReader::KeepFrom() const {
	if (lex_start == DConstants::INVALID_INDEX) {
		return window.bytes->offset;
	}
	idx_t keep = cursor.pos;
	if (extract && !probe_only && fields.row_count > held_rows) {
		keep = MinValue<idx_t>(keep, cursor.anchor + fields.rows[held_rows]);
		const idx_t begin = RowBegin(held_rows);
		if (begin != DConstants::INVALID_INDEX) {
			keep = MinValue(keep, begin);
		}
	}
	if (!extract && validate_count_utf8) {
		keep = MinValue(keep, validated_to);
	}
	if (lex_stop == DConstants::INVALID_INDEX) {
		const idx_t end = ranges.RangeEnd(range);
		keep = MinValue(keep, end - MinValue<idx_t>(end, 3));
	}
	return MaxValue(keep, window.bytes->offset);
}

void RowRangeReader::ReadPiece() {
	window.Read(KeepFrom());
	if (ranges.stream && !ranges.Intake()) {
		lex_stop = MinValue(lex_stop, ranges.Size());
	}
}

bool RowRangeReader::Waits() const {
	return lexer.Lookahead() == 0 && window.Waits();
}

bool RowRangeReader::CheckCountUtf8(idx_t end) {
	if (!validate_count_utf8) {
		return true;
	}
	if (!cursor.non_ascii) {
		validated_to = cursor.pos;
		return true;
	}
	if (end <= validated_to) {
		return true;
	}
	const auto &bytes = window.bytes;
	if (!simdutf::validate_utf8(bytes->data + (validated_to - bytes->offset), end - validated_to)) {
		return false;
	}
	validated_to = end;
	return true;
}

bool RowRangeReader::Lex() {
	const auto &bytes = window.bytes;
	const idx_t &read_to = window.read_to;
	const idx_t size = ranges.Size();
	const bool eof = read_to >= size;
	if (lex_start == DConstants::INVALID_INDEX) {
		idx_t found = ranges.start;
		if (range > 0) {
			const idx_t from = ranges.RangeBegin(range, size) - 1;
			found = read_to > from ? RowSync(dialect, bytes->data, bytes->offset, from, read_to, eof)
			                       : DConstants::INVALID_INDEX;
			if (found == DConstants::INVALID_INDEX) {
				if (!eof) {
					ReadPiece();
					return true;
				}
				found = size;
			}
		}
		if (lex_stop != DConstants::INVALID_INDEX) {
			found = MinValue(found, lex_stop);
		}
		lex_start = found;
		origin = found == ranges.start ? CSVState::NOT_SET : ranges.Origin(*bytes, found);
		if (found > ranges.start) {
			idx_t run = found;
			while (run > bytes->offset &&
			       (bytes->data[run - 1 - bytes->offset] == '\n' || bytes->data[run - 1 - bytes->offset] == '\r')) {
				run--;
			}
			line_end = run > bytes->offset ? run : DConstants::INVALID_INDEX;
		}
		lexer.Start(cursor, lex_start, range == 0);
		cursor.file_start = origin == CSVState::NOT_SET && ranges.file_start;
		cursor.after_cr = origin == CSVState::CARRIAGE_RETURN;
		validated_to = lex_start;
		return true;
	}
	if (lex_stop == DConstants::INVALID_INDEX) {
		const idx_t end = ranges.RangeEnd(range, size);
		if (end >= size) {
			lex_stop = size;
		} else if (read_to >= end) {
			const idx_t found = RowSync(dialect, bytes->data, bytes->offset, end - 1, read_to, eof);
			if (found != DConstants::INVALID_INDEX) {
				lex_stop = MaxValue(found, lex_start);
			} else if (eof) {
				lex_stop = size;
			}
		}
	}
	const idx_t limit = lex_stop != DConstants::INVALID_INDEX ? lex_stop : ranges.RangeEnd(range, size);
	idx_t feed_end = MinValue(limit, read_to);
	bool last = lex_stop != DConstants::INVALID_INDEX && feed_end == lex_stop;
	if (feed_end > cursor.pos + FEED_BYTES) {
		feed_end = cursor.pos + FEED_BYTES;
		last = false;
	}
	const bool whole = cursor.pos < feed_end && (last || feed_end - cursor.pos >= CSV_BLOCK_BYTES + lexer.Lookahead());
	const bool waits = !whole && cursor.pos < feed_end && feed_end == read_to && Waits();
	if (whole || waits) {
		auto out = extract && !probe_only ? &fields : nullptr;
		if (!lexer.Feed(cursor, bytes->data, bytes->offset, feed_end, last || waits, out)) {
			return false;
		}
		if (!extract &&
		    !CheckCountUtf8(cursor.last_term == DConstants::INVALID_INDEX ? lex_start : cursor.last_term + 1)) {
			return false;
		}
		if (out && !cursor.settled && read_to - KeepFrom() > UNSETTLED_BYTES) {
			probe_only = true;
			fields.Clear();
		}
		return true;
	}
	if (lex_stop != DConstants::INVALID_INDEX && cursor.pos == lex_stop) {
		return FinishRange();
	}
	ReadPiece();
	return true;
}

bool RowRangeReader::FinishRange() {
	CsvRangeEntry entry;
	auto out = extract && !probe_only ? &fields : nullptr;
	const bool eof = lex_stop == ranges.Size();
	const auto &bytes = window.bytes;
	if (!lexer.Finish(cursor, bytes->data, bytes->offset, lex_stop, eof, out, origin, entry)) {
		return false;
	}
	if (!extract && !CheckCountUtf8(lex_stop)) {
		return false;
	}
	if (!second_pass && !extending) {
		ranges.Publish(range, origin, entry);
	}
	const auto state = entry.end.states[1];
	open_end = !eof && state != CSVState::RECORD_SEPARATOR && state != CSVState::CARRIAGE_RETURN;
	finished = true;
	if (cursor.settled && open_end) {
		return Extend();
	}
	range_lines = cursor.lines;
	if (!extract) {
		counted_rows = cursor.rows;
	}
	return true;
}

void RowRangeReader::Rebase() {
	if (cursor.pos - cursor.anchor < REBASE_BYTES) {
		return;
	}
	const auto delta = fields.row_count ? fields.rows[0] : static_cast<uint32_t>(cursor.pos - cursor.anchor);
	if (delta > 0) {
		fields.Rebase(delta);
		cursor.anchor += delta;
	}
}

void RowRangeReader::OpenRowFits() {
	Rebase();
	if (cursor.pos - cursor.anchor >= ROW_LIMIT) {
		throw InvalidInputException(
		    "The row at byte %llu of \"%s\" is longer than %llu bytes, the longest row that can "
		    "be read",
		    cursor.anchor, ranges.path, ROW_LIMIT);
	}
}

bool RowRangeReader::Extend() {
	const auto &bytes = window.bytes;
	const idx_t &read_to = window.read_to;
	RowCursor probe = cursor;
	probe.first_term = DConstants::INVALID_INDEX;
	probe.first_term_cr = false;
	probe.check_arity = false;
	while (true) {
		const bool eof = read_to >= ranges.Size();
		if (probe.pos < read_to && (eof || read_to - probe.pos >= CSV_BLOCK_BYTES + lexer.Lookahead())) {
			[[maybe_unused]] const bool fed = lexer.Feed(probe, bytes->data, bytes->offset, read_to, eof, nullptr);
			D_ASSERT(fed);
			if (probe.first_term != DConstants::INVALID_INDEX) {
				break;
			}
			continue;
		}
		if (eof) {
			break;
		}
		ReadPiece();
	}
	idx_t stop = ranges.Size();
	if (probe.first_term != DConstants::INVALID_INDEX) {
		const idx_t first = probe.first_term;
		const auto at = [&](idx_t position) {
			return bytes->data[position - bytes->offset];
		};
		stop = first + 1;
		if (dialect.carry_on && at(first) == '\r' && stop < read_to && at(stop) == '\n') {
			stop++;
		}
	}
	lex_stop = stop;
	extending = true;
	open_end = false;
	finished = false;
	return true;
}

bool RowRangeReader::Recover() {
	range_lines = DConstants::INVALID_INDEX;
	counted_rows = DConstants::INVALID_INDEX;
	if (!cursor.settled && !second_pass) {
		active = false;
		return FromChain();
	}
	if (extract) {
		throw InternalException("The row lexer refused a verified range of \"%s\"", ranges.path);
	}
	force_extract = true;
	StartPass(true);
	return true;
}

RowReadResult RowRangeReader::Advance(idx_t want) {
	while (active) {
		const bool settled = cursor.settled;
		if (recover_pending && held_rows == 0) {
			recover_pending = false;
			if (!Recover()) {
				return RowReadResult::DONE;
			}
			continue;
		}
		if (finished && !settled) {
			const auto start = ranges.StartState(range);
			if (start.states[1] != origin) {
				active = false;
				range_lines = DConstants::INVALID_INDEX;
				counted_rows = DConstants::INVALID_INDEX;
				if (!FromChain()) {
					return RowReadResult::DONE;
				}
				continue;
			}
			cursor.settled = true;
			if (open_end && !probe_only) {
				Extend();
			}
			continue;
		}
		if (finished && probe_only) {
			StartPass(true);
			continue;
		}
		if (!extract) {
			if (finished) {
				active = false;
				return RowReadResult::COUNTED;
			}
			if (!Lex() && !Recover()) {
				return RowReadResult::DONE;
			}
			continue;
		}
		const idx_t ready = Ready();
		const bool full_span = cursor.pos - cursor.anchor >= REBASE_BYTES;
		if (settled && ready > held_rows &&
		    (ready >= want || finished || full_span || (cursor.pos == window.read_to && Waits()))) {
			return RowReadResult::ROWS;
		}
		if (settled && held_rows > 0 && ready == held_rows &&
		    (finished || full_span || recover_pending || (cursor.pos == window.read_to && Waits()))) {
			return RowReadResult::FLUSH;
		}
		if (finished) {
			active = false;
			return RowReadResult::DONE;
		}
		OpenRowFits();
		if (!Lex()) {
			if (held_rows > 0) {
				recover_pending = true;
				continue;
			}
			if (!Recover()) {
				return RowReadResult::DONE;
			}
		}
	}
	return RowReadResult::DONE;
}

void RowRangeReader::Emit(idx_t count) {
	last_emitted_end = RowEnd(count - 1);
	fields.Drop(count, count * columns);
	Rebase();
	emitted_rows += count;
	held_rows = 0;
}

idx_t RowRangeReader::RowEnd(idx_t row) const {
	const auto anomaly = fields.anomalies.empty() ? nullptr : fields.Anomaly(row);
	if (anomaly && anomaly->fields > columns) {
		return cursor.anchor + anomaly->end;
	}
	return cursor.anchor + (fields.ends[(row + 1) * columns - 1] & ROW_FIELD_MASK);
}

idx_t RowRangeReader::RowBegin(idx_t row) const {
	if (row > 0) {
		return RowEnd(row - 1);
	}
	return emitted_rows > 0 ? last_emitted_end : line_end;
}

idx_t RowRangeReader::LinesAt(idx_t position, idx_t start_lines) {
	if (position > cursor.pos) {
		return start_lines + cursor.lines;
	}
	const auto &bytes = window.bytes;
	if (lines_at == DConstants::INVALID_INDEX || position < lines_at) {
		const idx_t lexed = cursor.lines - (cursor.unterminated && !cursor.unfinished ? 1 : 0);
		lines_counted = lexed - lexer.CountLines(bytes->data, bytes->offset, position, cursor.pos) + start_lines;
	} else {
		lines_counted += lexer.CountLines(bytes->data, bytes->offset, lines_at, position);
	}
	lines_at = position;
	return lines_counted;
}

} // namespace duckdb
