//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/row_ranges.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/types/vector_buffer.hpp"
#include "duckdb/formats/row_chain.hpp"
#include "duckdb/formats/row_lexer.hpp"

namespace duckdb {

class RowBuffer {
public:
	virtual ~RowBuffer() = default;
};

struct RowBytes {
	shared_ptr<RowBuffer> buffer;
	AllocatedData owned;
	idx_t capacity = 0;
	const char *data = nullptr;
	idx_t offset = 0;
	idx_t size = 0;
};

class RowBytesHolder : public AuxiliaryDataHolder {
public:
	explicit RowBytesHolder(shared_ptr<RowBytes> bytes_p) : bytes(std::move(bytes_p)) {
	}

private:
	shared_ptr<RowBytes> bytes;
};

class RowSource {
public:
	virtual ~RowSource() = default;

	virtual idx_t Read(char *data, idx_t count, idx_t offset) const = 0;
	virtual void Release(idx_t position) {
	}
	virtual optional_idx End() const {
		return optional_idx();
	}
	virtual bool Live() const {
		return false;
	}
	virtual bool Available(idx_t offset) const {
		return true;
	}
	virtual void Nudge() {
	}
	idx_t ReadFully(char *data, idx_t count, idx_t offset) const;
};

struct RowRangeBounds {
	idx_t begin = 0;
	idx_t end = 0;
};

struct RowSearch {
	idx_t searched_to = 0;
	idx_t scanned_to = 0;
	bool quoted = false;
	bool plain = true;
	RowCursor probe;
};

struct RowClaim {
	idx_t begin = 0;
	shared_ptr<RowBytes> carry;
	idx_t carry_to = 0;
	CSVState origin = CSVState::NOT_SET;
	idx_t line_end = DConstants::INVALID_INDEX;
	bool resumed = false;
	RowSearch search;
};

idx_t RowSync(const RowDialect &dialect, const char *bytes, idx_t base, idx_t begin, idx_t end, bool eof);

class RowRanges {
public:
	static constexpr idx_t FEED_BYTES = 1 << 20;

	RowRanges(Allocator &allocator, const RowLexer &lexer, RowSource &source, idx_t start, idx_t size,
	          idx_t range_bytes, bool file_start);
	RowRanges(Allocator &allocator, RowSource &source, idx_t start, idx_t size, idx_t range_bytes);

	void Reset(idx_t size, idx_t range_bytes);
	bool Claim(idx_t &range);
	bool ClaimRead(idx_t range, idx_t &from);
	void PassRead(idx_t to);
	bool Idle(idx_t range, idx_t read_to) const;
	bool ClaimCut(idx_t range, RowClaim &claim);
	void Cut(idx_t end, shared_ptr<RowBytes> rest, idx_t read_to, CSVState origin, idx_t line_end);
	void CarryOn(shared_ptr<RowBytes> rest, idx_t read_to, const RowSearch &search, bool idle);
	void FailIntake();
	bool Finish(idx_t range);
	bool AllFinished() const;
	void Publish(idx_t range, CSVState origin, const CsvRangeEntry &entry);
	CsvRangeEntry Entry(idx_t range, CSVState origin) const;
	void Computing(idx_t range, bool value);
	CsvRangeStart StartState(idx_t range);
	idx_t Size() const {
		return size.load(std::memory_order_acquire);
	}
	idx_t RangeCount() const {
		return range_count.load(std::memory_order_acquire);
	}
	bool Intake() const {
		return intake;
	}
	void FoundEnd(idx_t end);
	idx_t RangeBegin(idx_t range, idx_t size_p) const {
		return MinValue(start + range * range_bytes, size_p);
	}
	idx_t RangeEnd(idx_t range, idx_t size_p) const {
		return MinValue(start + (range + 1) * range_bytes, size_p);
	}
	idx_t RangeBegin(idx_t range) const {
		return RangeBegin(range, Size());
	}
	idx_t RangeEnd(idx_t range) const {
		return RangeEnd(range, Size());
	}
	idx_t Read(char *data, idx_t count, idx_t offset) const {
		return source.Read(data, count, offset);
	}
	void ReadInto(RowBytes &bytes, idx_t offset, idx_t count);
	bool Empty(idx_t range, const RowBytes &bytes) const;
	void PublishEmpty(idx_t range, const RowBytes &bytes);
	RowRangeBounds Bounds(idx_t range, RowBytes &bytes);
	CSVState Origin(const RowBytes &bytes, idx_t begin) const;
	CsvRangeEntry Transfer(idx_t range, CSVState origin);
	CsvRangeEntry LexTransfer(const RowBytes &bytes, idx_t begin, idx_t end, CSVState origin) const;
	idx_t SyncAt(idx_t position);
	void SetWholeFile(shared_ptr<RowBuffer> buffer, const char *data);
	bool WholeFile() const {
		return whole_data != nullptr;
	}
	void ViewWholeFile(RowBytes &bytes) const;

public:
	Allocator &allocator;
	optional_ptr<const RowLexer> lexer;
	RowSource &source;
	const idx_t start;
	const bool stream;
	const bool file_start;
	idx_t range_bytes;
	idx_t feed_bytes = FEED_BYTES;
	string path;

private:
	void Layout(idx_t range_bytes);
	bool Complete() const;

private:
	mutable mutex lock;
	atomic<idx_t> size;
	atomic<idx_t> range_count;
	bool intake = false;
	shared_ptr<RowBuffer> whole_buffer;
	const char *whole_data = nullptr;
	idx_t next_range = 0;
	idx_t finished_ranges = 0;
	idx_t intake_begin = 0;
	shared_ptr<RowBytes> intake_carry;
	idx_t intake_carry_to = 0;
	CSVState intake_origin = CSVState::NOT_SET;
	idx_t intake_line_end = DConstants::INVALID_INDEX;
	bool intake_resumed = false;
	RowSearch intake_search;
	idx_t idle_from = 0;
	idx_t reading = 0;
	idx_t read_pos = 0;
	idx_t serving = 0;
	bool failed = false;
	RowChain<CsvRangeStart, CsvRangeEntry, CSV_RANGE_ORIGINS> chain;
};

enum class RowReadResult : uint8_t { ROWS, FLUSH, COUNTED, DONE };

class RowWindow {
public:
	static constexpr idx_t PADDING = 64;
	static constexpr idx_t PIECE = 1 << 16;

	explicit RowWindow(RowRanges &ranges);

	void Start(idx_t from, bool release);
	void StartAhead(idx_t from);
	shared_ptr<RowBytes> Tail(idx_t from) const;
	void Prepend(shared_ptr<RowBytes> carry, idx_t begin, idx_t from);
	void Read(idx_t keep_from, idx_t piece = PIECE);
	bool Waits() const;

public:
	RowRanges &ranges;
	shared_ptr<RowBytes> bytes;
	idx_t read_to = 0;

private:
	void Prepare(idx_t keep_from, idx_t need);

private:
	shared_ptr<RowBytes> spare;
	bool release = false;
};

class RowLookup {
public:
	explicit RowLookup(RowRanges &ranges);

	optional_idx Find(idx_t from, idx_t offset, idx_t want);
	void Extend(idx_t want);
	idx_t End() const {
		return window_offset + (window_end - window_begin);
	}
	bool Eof() const {
		return window_eof;
	}

public:
	shared_ptr<RowBytes> bytes;

private:
	void Read(idx_t from, idx_t want);

private:
	RowRanges &ranges;
	idx_t window_begin = 0;
	idx_t window_end = 0;
	idx_t window_offset = 0;
	bool window_eof = false;
};

class RowRangeReader {
public:
	explicit RowRangeReader(RowRanges &ranges);

	void Start(idx_t range, bool extract, bool release, bool validate_count_utf8);
	bool StartIntake(idx_t range, bool extract, bool release, bool validate_count_utf8);
	void Adopt(shared_ptr<RowBytes> bytes, const RowCursor &cursor);
	RowReadResult Advance(idx_t want);
	idx_t Ready() const {
		return fields.end_count / columns - (cursor.phase >= columns ? 1 : 0);
	}
	void Hold(idx_t rows) {
		held_rows = rows;
	}
	void Emit(idx_t count);
	idx_t RowEnd(idx_t row) const;
	idx_t RowBegin(idx_t row) const;
	idx_t LinesAt(idx_t position, idx_t start_lines);
	void ResetLines() {
		lines_at = DConstants::INVALID_INDEX;
	}
	bool Finished() const {
		return finished;
	}
	bool Active() const {
		return active;
	}
	idx_t CountedRows() const {
		return counted_rows;
	}
	idx_t RangeLines() const {
		return range_lines;
	}
	idx_t Range() const {
		return range;
	}
	idx_t Begin() const {
		return lex_start;
	}
	idx_t Stop() const {
		return lex_stop;
	}

public:
	RowRanges &ranges;
	const RowLexer &lexer;
	const RowDialect &dialect;
	const idx_t columns;
	RowWindow window;
	RowCursor cursor;
	RowFields fields;
	idx_t emitted_rows = 0;
	idx_t last_emitted_end = DConstants::INVALID_INDEX;
	idx_t line_end = DConstants::INVALID_INDEX;

private:
	void StartPass(bool second_pass);
	bool ReadChunk(idx_t from);
	void StartSearch(RowSearch &search) const;
	idx_t CutIntake(RowSearch &search, bool idle) const;
	void QuoteParityTerm(const RowBytes &bytes, idx_t from, idx_t read_to, bool quoted, RowCursor &last) const;
	idx_t LineEndBefore(const RowBytes &bytes, idx_t position, idx_t floor) const;
	void EscapeParityTerm(const RowBytes &bytes, idx_t from, idx_t limit, RowCursor &last) const;
	void PlainTerm(const RowBytes &bytes, idx_t from, idx_t limit, RowCursor &last) const;
	idx_t IntakeCut(const RowCursor &probe) const;
	void PublishOwnTransfer();
	bool FromChain();
	void FinishEmpty();
	bool Lex();
	bool FinishRange();
	bool Extend();
	void Rebase();
	void OpenRowFits();
	void ReadPiece(idx_t piece);
	bool Waits() const;
	idx_t KeepFrom() const;
	bool CheckCountUtf8(idx_t end);
	bool Recover();

private:
	idx_t range = 0;
	bool intake = false;
	bool active = false;
	bool extract = false;
	bool extract_rows = false;
	bool recover_pending = false;
	bool release = false;
	bool validate_count_utf8 = false;
	bool probe_only = false;
	bool finished = false;
	bool second_pass = false;
	bool extending = false;
	bool open_end = false;
	bool force_extract = false;
	CSVState origin = CSVState::NOT_SET;
	idx_t lex_start = 0;
	idx_t lex_stop = 0;
	idx_t validated_to = 0;
	idx_t held_rows = 0;
	idx_t counted_rows = DConstants::INVALID_INDEX;
	idx_t range_lines = DConstants::INVALID_INDEX;
	idx_t lines_at = DConstants::INVALID_INDEX;
	idx_t lines_counted = 0;
};

} // namespace duckdb
