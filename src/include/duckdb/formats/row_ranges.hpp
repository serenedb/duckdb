//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/row_ranges.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/types/vector_buffer.hpp"
#include "duckdb/formats/row_lexer.hpp"

namespace duckdb {

class RowBuffer {
public:
	virtual ~RowBuffer() = default;
};

struct RowBytes {
	shared_ptr<RowBuffer> buffer;
	unsafe_unique_array<char> owned;
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
};

struct RowRangeBounds {
	idx_t begin = 0;
	idx_t end = 0;
};

idx_t RowSync(const RowDialect &dialect, const char *bytes, idx_t base, idx_t begin, idx_t end, bool eof);

class RowRanges {
public:
	RowRanges(const RowLexer &lexer, RowSource &source, idx_t start, idx_t size, idx_t range_bytes, bool file_start);

	bool Claim(idx_t &range);
	bool Finish(idx_t range);
	bool AllFinished() const;
	void Publish(idx_t range, CSVState origin, const CsvRangeEntry &entry);
	CsvRangeEntry Entry(idx_t range, CSVState origin) const;
	void Computing(idx_t range, bool value);
	CsvRangeStart StartState(idx_t range);
	idx_t RangeBegin(idx_t range) const {
		return MinValue(start + range * range_bytes, size);
	}
	idx_t RangeEnd(idx_t range) const {
		return MinValue(start + (range + 1) * range_bytes, size);
	}
	idx_t Read(char *data, idx_t count, idx_t offset) const {
		return source.Read(data, count, offset);
	}
	void ReadInto(RowBytes &bytes, idx_t offset, idx_t count) const;
	RowRangeBounds Bounds(idx_t range, RowBytes &bytes) const;
	CSVState Origin(const RowBytes &bytes, idx_t begin) const;
	CsvRangeEntry Transfer(idx_t range, CSVState origin) const;
	CsvRangeEntry LexTransfer(const RowBytes &bytes, idx_t begin, idx_t end, CSVState origin) const;
	idx_t SyncAt(idx_t position) const;
	void SetWholeFile(shared_ptr<RowBuffer> buffer, const char *data);
	bool WholeFile() const {
		return whole_data != nullptr;
	}
	void ViewWholeFile(RowBytes &bytes) const;

public:
	const RowLexer &lexer;
	RowSource &source;
	const idx_t start;
	idx_t size;
	const bool stream;
	const bool file_start;
	idx_t range_bytes;
	idx_t range_count;
	string path;

private:
	mutable mutex lock;
	shared_ptr<RowBuffer> whole_buffer;
	const char *whole_data = nullptr;
	idx_t next_range = 0;
	idx_t finished_ranges = 0;
	idx_t resolved = 0;
	vector<CsvRangeTransfer> transfers;
	vector<bool> computing;
	vector<CsvRangeStart> starts;
};

enum class RowReadResult : uint8_t { ROWS, FLUSH, COUNTED, DONE };

class RowRangeReader {
public:
	explicit RowRangeReader(RowRanges &ranges);

	void Start(idx_t range, bool extract, bool release, bool validate_count_utf8);
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
	idx_t Stop() const {
		return lex_stop;
	}

public:
	RowRanges &ranges;
	const RowLexer &lexer;
	const RowDialect &dialect;
	const idx_t columns;
	shared_ptr<RowBytes> bytes;
	RowCursor cursor;
	RowFields fields;
	idx_t emitted_rows = 0;
	idx_t last_emitted_end = DConstants::INVALID_INDEX;
	idx_t line_end = DConstants::INVALID_INDEX;

private:
	void StartPass(bool second_pass);
	void PublishOwnTransfer();
	bool FromChain();
	bool Lex();
	bool FinishRange();
	bool Extend();
	void Rebase();
	void OpenRowFits();
	void ReadPiece();
	void PrepareWindow(idx_t keep_from, idx_t need);
	idx_t KeepFrom() const;
	bool CheckCountUtf8(idx_t end);
	bool Recover();

private:
	shared_ptr<RowBytes> spare;
	idx_t range = 0;
	bool active = false;
	bool extract = false;
	bool extract_rows = false;
	bool recover_pending = false;
	bool release = false;
	bool validate_count_utf8 = false;
	bool probe_only = false;
	bool finished = false;
	bool verified = false;
	bool second_pass = false;
	bool extending = false;
	bool open_end = false;
	bool force_extract = false;
	CSVState origin = CSVState::NOT_SET;
	idx_t lex_start = 0;
	idx_t lex_stop = 0;
	idx_t read_to = 0;
	idx_t validated_to = 0;
	idx_t held_rows = 0;
	idx_t counted_rows = DConstants::INVALID_INDEX;
	idx_t range_lines = DConstants::INVALID_INDEX;
	idx_t lines_at = DConstants::INVALID_INDEX;
	idx_t lines_counted = 0;
};

} // namespace duckdb
