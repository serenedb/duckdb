#include "duckdb/formats/row_ranges.hpp"

#include "absl/cleanup/cleanup.h"
#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/fault_points.hpp"
#include "duckdb/formats/csv_simd.hpp"
#include "simdutf.h"

namespace duckdb {

namespace {

constexpr idx_t RANGE_OVERLAP = 1 << 12;
constexpr idx_t SYNC_WINDOW = 1 << 14;
constexpr idx_t READ_PIECE = 1 << 16;
constexpr idx_t WINDOW_BYTES = 1 << 20;
constexpr idx_t UNSETTLED_BYTES = 1 << 22;
constexpr idx_t FEED_BYTES = 1 << 20;
constexpr idx_t REBASE_BYTES = 1 << 29;
constexpr idx_t ROW_LIMIT = idx_t(3) << 29;

char LastDelimiterByte(const RowDialect &dialect) {
	return dialect.delimiter_size > 1 ? dialect.delimiter_tail[dialect.delimiter_size - 2] : dialect.delimiter;
}

bool QuotedEscape(const RowDialect &dialect) {
	return dialect.escape != '\0' && dialect.escape != dialect.quote;
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

idx_t RowSync(const RowDialect &dialect, const char *bytes, idx_t base, idx_t begin, idx_t end, bool eof) {
	idx_t first = FirstNewline(dialect, bytes, base, begin, end, eof);
	if (first != DConstants::INVALID_INDEX && dialect.backslash) {
		while (first < end) {
			const char byte = bytes[first - base];
			if (byte == '\n') {
				first++;
			} else if (byte == '\r' && first + 1 < end && bytes[first + 1 - base] == '\n') {
				first += 2;
			} else if (byte == '\r' && first + 1 == end && !eof) {
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

RowRanges::RowRanges(const RowLexer &lexer_p, RowSource &source_p, idx_t start_p, idx_t size_p, idx_t range_bytes_p,
                     bool file_start_p)
    : lexer(lexer_p), source(source_p), start(start_p), stream(size_p == NumericLimits<idx_t>::Maximum()),
      file_start(file_start_p), size(size_p) {
	Layout(range_bytes_p);
}

void RowRanges::Reset(idx_t size_p, idx_t range_bytes_p) {
	D_ASSERT(!stream && size_p != NumericLimits<idx_t>::Maximum());
	const absl::MutexLock guard(&lock);
	size.store(size_p, std::memory_order_release);
	next_range = 0;
	finished_ranges = 0;
	resolved = 0;
	Layout(range_bytes_p);
}

void RowRanges::Layout(idx_t range_bytes_p) {
	const idx_t end = size.load(std::memory_order_relaxed);
	dynamic = stream && range_bytes_p != DConstants::INVALID_INDEX;
	idx_t count;
	if (dynamic) {
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
	const idx_t slots = dynamic ? 1 : count;
	transfers.assign(slots, CsvRangeTransfer());
	computing.assign(slots, 0);
	done.assign(dynamic ? slots : 0, false);
	starts.assign(slots + 1, CsvRangeStart());
	starts[0].line_end = start;
	starts[0].last_position = start;
	unfinished = 0;
	released = 0;
}

void RowRanges::Grow(idx_t count) {
	if (transfers.size() >= count) {
		return;
	}
	transfers.resize(count);
	computing.resize(count, 0);
	done.resize(count, false);
	starts.resize(count + 1);
}

void RowRanges::FoundEnd(idx_t end) {
	const absl::MutexLock guard(&lock);
	if (end >= size.load(std::memory_order_relaxed)) {
		return;
	}
	size.store(end, std::memory_order_release);
	if (dynamic) {
		const idx_t data_size = end - MinValue(start, end);
		range_count.store(MaxValue<idx_t>((data_size + range_bytes - 1) / range_bytes, 1), std::memory_order_release);
	}
}

void RowRanges::ReleaseResolved() {
	idx_t mark = MinValue(unfinished, resolved);
	for (idx_t range = released; range < mark; range++) {
		if (computing[range] != 0) {
			mark = range;
			break;
		}
	}
	if (mark <= released) {
		return;
	}
	released = mark;
	const idx_t begin = MinValue(start + mark * range_bytes, size.load(std::memory_order_relaxed));
	if (begin > start + 3) {
		source.Release(begin - 3);
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
	if (bytes.capacity < count || !bytes.owned) {
		bytes.capacity = MaxValue<idx_t>(count, 1);
		bytes.owned = make_unsafe_uniq_array_uninitialized<char>(bytes.capacity);
	}
	bytes.buffer.reset();
	bytes.data = bytes.owned.get();
	bytes.offset = offset;
	bytes.size = count ? Read(bytes.owned.get(), count, offset) : 0;
	if (stream && bytes.size < count) {
		const auto known = source.End();
		if (known.IsValid()) {
			FoundEnd(known.GetIndex());
		} else if (bytes.size > 0) {
			FoundEnd(offset + bytes.size);
		}
	}
}

void RowRanges::ViewWholeFile(RowBytes &bytes) const {
	bytes.owned.reset();
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
			const idx_t found = RowSync(lexer.Dialect(), bytes.data, bytes.offset, position, available, eof);
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
	return newline == '\r' && lexer.Dialect().carry_on ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR;
}

CsvRangeEntry RowRanges::Transfer(idx_t range, CSVState origin) {
	RowBytes bytes;
	const auto bounds = Bounds(range, bytes);
	return LexTransfer(bytes, bounds.begin, bounds.end, origin);
}

CsvRangeEntry RowRanges::LexTransfer(const RowBytes &bytes, idx_t begin, idx_t end, CSVState origin) const {
	RowCursor cursor;
	lexer.Start(cursor, begin, true, false);
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
	lexer.Feed(cursor, bytes.data, bytes.offset, end, true, nullptr);
	CsvRangeEntry entry;
	lexer.Finish(cursor, bytes.data, bytes.offset, end, end >= Size(), nullptr, origin, entry);
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
		const idx_t found = RowSync(lexer.Dialect(), bytes.data, bytes.offset, position, available, eof);
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
	if (next_range >= range_count.load(std::memory_order_relaxed)) {
		return false;
	}
	range = next_range++;
	if (dynamic) {
		Grow(next_range);
	}
	return true;
}

bool RowRanges::Complete() const {
	return finished_ranges == MaxValue(range_count.load(std::memory_order_relaxed), next_range);
}

bool RowRanges::Finish(idx_t range) {
	const absl::MutexLock guard(&lock);
	finished_ranges++;
	if (dynamic) {
		done[range] = true;
		while (unfinished < next_range && done[unfinished]) {
			unfinished++;
		}
		ReleaseResolved();
	}
	return Complete();
}

bool RowRanges::AllFinished() const {
	const absl::MutexLock guard(&lock);
	return Complete();
}

void RowRanges::Computing(idx_t range, bool value) {
	const absl::MutexLock guard(&lock);
	if (value) {
		computing[range]++;
		return;
	}
	D_ASSERT(computing[range] > 0);
	computing[range]--;
	if (dynamic) {
		ReleaseResolved();
	}
}

void RowRanges::Publish(idx_t range, CSVState origin, const CsvRangeEntry &entry) {
	const absl::MutexLock guard(&lock);
	auto &slot = transfers[range][static_cast<uint8_t>(origin)];
	if (!slot.computed) {
		slot = entry;
	}
	const idx_t known = MinValue<idx_t>(range_count.load(std::memory_order_relaxed), transfers.size());
	const idx_t before = resolved;
	while (resolved < known) {
		auto &current = starts[resolved];
		auto &found = transfers[resolved][static_cast<uint8_t>(current.states[1])];
		if (!found.computed) {
			break;
		}
		auto &next = starts[resolved + 1];
		next = found.end;
		next.Inherit(current);
		resolved++;
	}
	if (dynamic && resolved != before) {
		ReleaseResolved();
	}
}

CsvRangeEntry RowRanges::Entry(idx_t range, CSVState origin) const {
	const absl::MutexLock guard(&lock);
	return transfers[range][static_cast<uint8_t>(origin)];
}

CsvRangeStart RowRanges::StartState(idx_t range) {
	while (true) {
		idx_t missing;
		CSVState origin;
		{
			const absl::MutexLock guard(&lock);
			auto ready = [&]() {
				return resolved >= range || computing[resolved] == 0;
			};
			lock.Await(absl::Condition(&ready));
			if (resolved >= range) {
				return starts[range];
			}
			missing = resolved;
			origin = starts[missing].states[1];
			computing[missing]++;
		}
		absl::Cleanup stop = [&] {
			Computing(missing, false);
		};
		Publish(missing, origin, Transfer(missing, origin));
	}
}

RowRangeReader::RowRangeReader(RowRanges &ranges_p)
    : ranges(ranges_p), lexer(ranges_p.lexer), dialect(ranges_p.lexer.Dialect()), columns(ranges_p.lexer.Columns()) {
}

void RowRangeReader::Start(idx_t range_p, bool extract_p, bool release_p, bool validate_count_utf8_p) {
	range = range_p;
	extract_rows = extract_p;
	release = release_p;
	validate_count_utf8 = validate_count_utf8_p;
	force_extract = false;
	recover_pending = false;
	StartPass(false);
}

void RowRangeReader::Adopt(shared_ptr<RowBytes> bytes_p, const RowCursor &cursor_p) {
	bytes = std::move(bytes_p);
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
	beyond = false;
	exact_end = second_pass_p || range == 0;
	finished = false;
	verified = false;
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
	const idx_t begin = ranges.RangeBegin(range);
	const idx_t from = second_pass ? lex_start : (range == 0 ? ranges.start : begin - MinValue<idx_t>(begin, 3));
	if (!bytes || bytes.use_count() > 1) {
		bytes = make_shared_ptr<RowBytes>();
	}
	if (ranges.WholeFile()) {
		ranges.ViewWholeFile(*bytes);
		read_to = ranges.Size();
	} else {
		if (bytes->buffer) {
			bytes = make_shared_ptr<RowBytes>();
		}
		bytes->offset = from;
		bytes->size = 0;
		bytes->data = bytes->owned.get();
		read_to = from;
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
		return bytes->offset;
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
	return MaxValue(keep, bytes->offset);
}

void RowRangeReader::PrepareWindow(idx_t keep_from, idx_t need) {
	auto &window = *bytes;
	const idx_t kept = read_to - keep_from;
	const bool fits = window.owned && read_to - window.offset + need <= window.capacity;
	if (bytes.use_count() == 1 && window.owned && window.capacity >= kept + need) {
		if (keep_from > window.offset && (kept <= READ_PIECE || !fits)) {
			memmove(window.owned.get(), window.owned.get() + (keep_from - window.offset), kept);
			window.offset = keep_from;
			window.size = kept;
		}
		window.data = window.owned.get();
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
	if (!next->owned || next->capacity < kept + need) {
		const idx_t capacity = MaxValue(kept + need, MaxValue(WINDOW_BYTES, window.capacity * 2));
		next->owned = make_unsafe_uniq_array_uninitialized<char>(capacity);
		next->capacity = capacity;
	}
	if (kept) {
		memcpy(next->owned.get(), window.data + (keep_from - window.offset), kept);
	}
	next->buffer.reset();
	next->data = next->owned.get();
	next->offset = keep_from;
	next->size = kept;
	if (bytes.use_count() > 1) {
		spare = std::move(bytes);
	}
	bytes = std::move(next);
}

void RowRangeReader::ReadPiece() {
	const idx_t end = ranges.Size();
	const idx_t want = end > read_to ? MinValue<idx_t>(READ_PIECE, end - read_to) : 0;
	PrepareWindow(KeepFrom(), want);
	auto &window = *bytes;
	const idx_t got = want ? ranges.Read(window.owned.get() + (read_to - window.offset), want, read_to) : 0;
	window.size += got;
	read_to += got;
	if (!ranges.stream) {
		return;
	}
	if (got < want) {
		const auto known = ranges.source.End();
		if (known.IsValid()) {
			ranges.FoundEnd(known.GetIndex());
		} else if (got > 0 || exact_end) {
			ranges.FoundEnd(read_to);
		} else {
			beyond = true;
		}
	}
	exact_end = exact_end || got > 0;
	if (!ranges.Dynamic()) {
		if (release) {
			ranges.source.Release(read_to);
		}
		lex_stop = MinValue(lex_stop, ranges.Size());
	}
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
	if (!simdutf::validate_utf8(bytes->data + (validated_to - bytes->offset), end - validated_to)) {
		return false;
	}
	validated_to = end;
	return true;
}

bool RowRangeReader::Lex() {
	SDB_PARK_ONCE_ON_FAILURE("row_range_lex");
	const idx_t size = ranges.Size();
	const bool eof = read_to >= size;
	if (lex_start == DConstants::INVALID_INDEX) {
		idx_t found = ranges.start;
		if (range > 0) {
			const idx_t begin = ranges.RangeBegin(range, size);
			if (begin >= size) {
				beyond = true;
				return true;
			}
			const idx_t from = begin - 1;
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
	if (cursor.pos < feed_end && (last || feed_end - cursor.pos >= CSV_BLOCK_BYTES + lexer.Lookahead())) {
		auto out = extract && !probe_only ? &fields : nullptr;
		if (!lexer.Feed(cursor, bytes->data, bytes->offset, feed_end, last, out)) {
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
	if ((cursor.settled || verified) && open_end) {
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
	RowCursor probe = cursor;
	probe.first_term = DConstants::INVALID_INDEX;
	probe.first_term_cr = false;
	probe.check_arity = false;
	while (true) {
		const bool eof = read_to >= ranges.Size();
		if (probe.pos < read_to && (eof || read_to - probe.pos >= CSV_BLOCK_BYTES + lexer.Lookahead())) {
			lexer.Feed(probe, bytes->data, bytes->offset, read_to, eof, nullptr);
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
	const bool settled = cursor.settled || verified;
	if (!settled && !second_pass) {
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
		if (lex_start == DConstants::INVALID_INDEX && range > 0 &&
		    (beyond || ranges.RangeBegin(range) >= ranges.Size())) {
			active = false;
			finished = true;
			range_lines = 0;
			counted_rows = 0;
			return extract ? RowReadResult::DONE : RowReadResult::COUNTED;
		}
		const bool settled = cursor.settled || verified;
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
			verified = true;
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
		if (settled && ready > held_rows && (ready >= want || finished || full_span)) {
			return RowReadResult::ROWS;
		}
		if (settled && held_rows > 0 && ready == held_rows && (finished || full_span || recover_pending)) {
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
