#include "absl/synchronization/mutex.h"
#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_buffer_manager.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/memory_mapped_file.hpp"
#include "duckdb/formats/range_lines.hpp"
#include "duckdb/formats/row_input.hpp"
#include "duckdb/formats/row_lexer.hpp"
#include "duckdb/formats/row_ranges.hpp"
#include "duckdb/formats/row_stream.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include <thread>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace duckdb;

namespace {

class PlainRowStream : public RowSource {
public:
	explicit PlainRowStream(string data_p) : data(std::move(data_p)) {
	}

	idx_t Read(char *out, idx_t count, idx_t offset) const override {
		if (offset >= data.size()) {
			return 0;
		}
		const idx_t size = MinValue<idx_t>(count, data.size() - offset);
		memcpy(out, data.data() + offset, size);
		return size;
	}

private:
	string data;
};

CsvGrammar TestGrammar(char delimiter, bool strict, NewLineIdentifier new_line) {
	CsvGrammar grammar;
	grammar.delimiter[0] = delimiter;
	grammar.quote = '"';
	grammar.escape = '"';
	grammar.new_line = new_line;
	grammar.strict_mode = strict;
	grammar.carry_on_newline = new_line == NewLineIdentifier::CARRY_ON;
	grammar.empty_space = delimiter != ' ';
	grammar.trim_quoted_spaces = grammar.empty_space;
	return grammar;
}

class RowRangesTestStream : public RowSource {
public:
	explicit RowRangesTestStream(string data_p) : data(std::move(data_p)) {
	}

	idx_t Read(char *out, idx_t count, idx_t offset) const override {
		if (offset >= data.size()) {
			return 0;
		}
		const idx_t size = MinValue<idx_t>(count, data.size() - offset);
		memcpy(out, data.data() + offset, size);
		return size;
	}

	optional_idx End() const override {
		return data.size();
	}

private:
	string data;
};

class CountingRowSource : public RowRangesTestStream {
public:
	using RowRangesTestStream::RowRangesTestStream;

	idx_t Read(char *out, idx_t count, idx_t offset) const override {
		const idx_t got = RowRangesTestStream::Read(out, count, offset);
		read += got;
		return got;
	}

	mutable idx_t read = 0;
};

class LiveRowStream : public RowSource {
public:
	void Append(std::string_view bytes) {
		const absl::MutexLock guard(&lock);
		data.append(bytes);
	}

	void Close() {
		const absl::MutexLock guard(&lock);
		closed = true;
	}

	void AwaitReader() {
		const auto drained = [&]() {
			return waiting_at != DConstants::INVALID_INDEX && waiting_at >= data.size();
		};
		lock.LockWhen(absl::Condition(&drained));
		lock.Unlock();
	}

	void Release(idx_t position) override {
		const absl::MutexLock guard(&lock);
		released = MaxValue(released, position);
	}

	idx_t Read(char *out, idx_t count, idx_t offset) const override {
		const absl::MutexLock guard(&lock);
		if (offset < released) {
			read_released = true;
		}
		waiting_at = offset;
		const auto ready = [&]() {
			return offset < data.size() || closed;
		};
		lock.Await(absl::Condition(&ready));
		waiting_at = DConstants::INVALID_INDEX;
		if (offset >= data.size()) {
			return 0;
		}
		const idx_t size = MinValue<idx_t>(count, data.size() - offset);
		memcpy(out, data.data() + offset, size);
		return size;
	}

	optional_idx End() const override {
		const absl::MutexLock guard(&lock);
		return closed ? optional_idx(data.size()) : optional_idx();
	}

	bool Live() const override {
		return true;
	}

	bool Available(idx_t offset) const override {
		const absl::MutexLock guard(&lock);
		return offset < data.size() || closed;
	}

	bool ReadReleased() const {
		const absl::MutexLock guard(&lock);
		return read_released;
	}

private:
	mutable absl::Mutex lock;
	mutable idx_t waiting_at = DConstants::INVALID_INDEX;
	mutable bool read_released = false;
	string data;
	idx_t released = 0;
	bool closed = false;
};

class MemoryRowSource : public RowSource {
public:
	explicit MemoryRowSource(const string &data_p) : data(data_p), size(data_p.size()) {
		data.append(RowWindow::PADDING, '\0');
	}

	idx_t Read(char *out, idx_t count, idx_t offset) const override {
		const idx_t available = offset < size ? MinValue(count, size - offset) : 0;
		memcpy(out, data.data() + offset, available);
		return available;
	}
	RowMemory Memory() const override {
		return {make_shared_ptr<RowBuffer>(), data.data()};
	}
	void Want(idx_t begin, idx_t end) override {
		wants.emplace_back(begin, end);
	}
	void ReadInPlace(idx_t begin, idx_t end) override {
		reads.emplace_back(begin, end);
	}
	void Done(idx_t begin, idx_t end) override {
		dones.emplace_back(begin, end);
	}
	optional_idx End() const override {
		return size;
	}

	vector<pair<idx_t, idx_t>> wants;
	vector<pair<idx_t, idx_t>> reads;
	vector<pair<idx_t, idx_t>> dones;

private:
	string data;
	idx_t size;
};

class TestMapping : public MemoryMappedFile {
public:
	TestMapping(data_ptr_t data_p, idx_t size_p)
	    : MemoryMappedFile("test", FileFlags::FILE_FLAGS_READ, data_p, size_p) {
	}

	void Sync() override {
	}
	bool Trim(idx_t offset, idx_t length) override {
		return false;
	}
	void Close() override {
	}
};

class RecordingMapping : public TestMapping {
public:
	using TestMapping::TestMapping;

	void Prefetch(idx_t location, idx_t nr_bytes) override {
		prefetched.emplace_back(location, nr_bytes);
	}
	void Release(idx_t location, idx_t nr_bytes) override {
		released.emplace_back(location, nr_bytes);
	}
	bool IsCached(idx_t location, idx_t nr_bytes) override {
		return cached;
	}

	bool cached = false;
	vector<pair<idx_t, idx_t>> prefetched;
	vector<pair<idx_t, idx_t>> released;
};

struct LexedRow {
	idx_t begin;
	vector<uint32_t> ends;

	bool operator==(const LexedRow &other) const {
		return begin == other.begin && ends == other.ends;
	}
};

void TakeRows(RowRangeReader &reader, vector<LexedRow> &rows) {
	const idx_t ready = reader.Ready();
	const idx_t columns = reader.columns;
	for (idx_t row = 0; row < ready; row++) {
		LexedRow lexed {reader.RowBegin(row), {}};
		for (idx_t column = 0; column < columns; column++) {
			const uint32_t end = reader.fields.ends[row * columns + column];
			lexed.ends.push_back(static_cast<uint32_t>(reader.cursor.anchor + (end & ROW_FIELD_MASK)) |
			                     (end & ROW_FIELD_FLAG));
		}
		rows.push_back(std::move(lexed));
	}
	reader.Emit(ready);
}

vector<LexedRow> ReadRows(const RowLexer &lexer, RowSource &source) {
	RowRanges ranges(Allocator::DefaultAllocator(), lexer, source, 0, NumericLimits<idx_t>::Maximum(),
	                 DConstants::INVALID_INDEX, true);
	idx_t range = 0;
	REQUIRE(ranges.Claim(range));
	RowRangeReader reader(ranges);
	reader.Start(range, true, false, false);
	vector<LexedRow> rows;
	while (true) {
		const auto result = reader.Advance(STANDARD_VECTOR_SIZE);
		if (result == RowReadResult::DONE) {
			return rows;
		}
		REQUIRE(result == RowReadResult::ROWS);
		TakeRows(reader, rows);
	}
}

vector<LexedRow> ReadRanges(const RowLexer &lexer, RowSource &source, idx_t range_bytes) {
	RowRanges ranges(Allocator::DefaultAllocator(), lexer, source, 0, NumericLimits<idx_t>::Maximum(), range_bytes,
	                 true);
	RowRangeReader reader(ranges);
	vector<LexedRow> rows;
	idx_t range = 0;
	while (ranges.Claim(range)) {
		if (!reader.StartIntake(range, true, true, false)) {
			continue;
		}
		while (true) {
			const auto result = reader.Advance(STANDARD_VECTOR_SIZE);
			if (result == RowReadResult::DONE) {
				break;
			}
			REQUIRE(result == RowReadResult::ROWS);
			TakeRows(reader, rows);
		}
		ranges.Finish(range);
	}
	return rows;
}

void ReadPiece(RowRanges &ranges, RowRangeReader &reader, vector<LexedRow> &rows, idx_t expected) {
	idx_t range = 0;
	REQUIRE(ranges.Claim(range));
	REQUIRE(reader.StartIntake(range, true, false, false));
	REQUIRE(reader.Advance(STANDARD_VECTOR_SIZE) == RowReadResult::ROWS);
	REQUIRE(reader.Ready() == expected);
	TakeRows(reader, rows);
	REQUIRE(reader.Advance(STANDARD_VECTOR_SIZE) == RowReadResult::DONE);
	ranges.Finish(range);
}

idx_t ScanLineBreak(const RowDialect &dialect, const string &data, idx_t base, idx_t begin, idx_t end, bool eof) {
	const auto at = [&](idx_t position) {
		return data[position - base];
	};
	const bool escapes = dialect.escape != '\0' && dialect.escape != dialect.quote;
	for (idx_t position = begin; position < end; position++) {
		const char byte = at(position);
		if (dialect.carriage_terms ? byte != '\n' && byte != '\r' : byte != dialect.line_break) {
			continue;
		}
		if (dialect.carriage_terms && byte == '\r') {
			if (position + 1 >= end) {
				if (!eof) {
					return DConstants::INVALID_INDEX;
				}
			} else if (at(position + 1) == '\n') {
				continue;
			}
		}
		if (escapes && position > base && at(position - 1) == dialect.escape) {
			continue;
		}
		if (escapes && position > base + 1 && at(position - 1) == '\r' && at(position - 2) == dialect.escape) {
			continue;
		}
		return position + 1;
	}
	return DConstants::INVALID_INDEX;
}

string LineitemRow(std::mt19937_64 &rng, char delimiter) {
	const char *words[] = {"ironic", "deposits", "a,b", "slyly \"bold\"", "final", "x"};
	string comment;
	for (idx_t i = 0; i < 1 + rng() % 6; i++) {
		comment += words[rng() % 6];
		comment += ' ';
	}
	if (delimiter == '\t') {
		for (auto &c : comment) {
			c = c == ',' || c == '"' ? ';' : c;
		}
	} else if (comment.find_first_of(",\"") != string::npos) {
		string quoted = "\"";
		for (const char c : comment) {
			quoted += c;
			if (c == '"') {
				quoted += '"';
			}
		}
		comment = quoted + "\"";
	}
	string row;
	for (idx_t column = 0; column < 15; column++) {
		row += to_string(rng() % 100000);
		row += column == 10 ? "-01-02" : "";
		row += delimiter;
	}
	return row + comment + "\n";
}

string RandomField(std::mt19937_64 &rng, const RowDialect &dialect) {
	string field;
	const idx_t size = rng() % 6;
	if (dialect.backslash) {
		const char *tokens[] = {"a", "b", " ", "\\\\", "\\\t", "\\\n", "\\N", "\\x41"};
		for (idx_t i = 0; i < size; i++) {
			field += tokens[rng() % 8];
		}
		return field;
	}
	const char plain[] = {'a', 'b', ' '};
	const char special[] = {'a', ',', '\n', '"', ' '};
	if (rng() % 2) {
		for (idx_t i = 0; i < size; i++) {
			field += plain[rng() % 3];
		}
		return field;
	}
	field += '"';
	for (idx_t i = 0; i < size; i++) {
		const char c = special[rng() % 5];
		if (c == '"') {
			field += dialect.escape;
		}
		field += c;
	}
	field += '"';
	return field;
}

} // namespace

TEST_CASE("A range reader hands over the rows of a live stream before the stream ends", "[csv]") {
	RowLexer lexer(RowDialect::PostgresText('\t'), 2, NumericLimits<idx_t>::Maximum(), true, false, true);
	LiveRowStream source;
	RowRanges ranges(Allocator::DefaultAllocator(), lexer, source, 0, NumericLimits<idx_t>::Maximum(), 1 << 21, true);
	RowRangeReader reader(ranges);
	vector<LexedRow> rows;
	source.Append("1\t2\n3\t4\n5\t");
	ReadPiece(ranges, reader, rows, 2);
	source.Append("6\n7\t8");
	ReadPiece(ranges, reader, rows, 1);
	source.Close();
	ReadPiece(ranges, reader, rows, 1);
	idx_t range = 0;
	REQUIRE(!ranges.Claim(range));
	REQUIRE(ranges.AllFinished());
	REQUIRE(rows.size() == 4);
	REQUIRE(rows[2].ends == vector<uint32_t> {9, 11});
	REQUIRE(rows[3].ends == vector<uint32_t> {13, 15});
}

TEST_CASE("Rows of a live stream read in ranges match the rows of the whole input", "[csv]") {
	std::mt19937_64 rng(43);
	for (idx_t round = 0; round < 40; round++) {
		const bool text = round % 2;
		const auto dialect = text ? RowDialect::PostgresText('\t') : RowDialect::PostgresCsv(',', '"', '"');
		string input;
		for (idx_t row = 0; row < 200 + rng() % 400; row++) {
			input += LineitemRow(rng, text ? '\t' : ',');
		}
		RowLexer lexer(dialect, 16, NumericLimits<idx_t>::Maximum(), true, false, true);
		RowRangesTestStream whole(input);
		const auto expected = ReadRows(lexer, whole);

		LiveRowStream live;
		vector<LexedRow> rows;
		const idx_t range_bytes = 256 << (rng() % 6);
		std::thread reader_thread([&]() { rows = ReadRanges(lexer, live, range_bytes); });
		for (idx_t at = 0; at < input.size();) {
			const idx_t piece = MinValue<idx_t>(1 + rng() % 3000, input.size() - at);
			live.AwaitReader();
			live.Append(std::string_view(input).substr(at, piece));
			at += piece;
		}
		live.AwaitReader();
		live.Close();
		reader_thread.join();
		INFO("round " << round << " range bytes " << range_bytes);
		REQUIRE(!live.ReadReleased());
		REQUIRE(rows.size() == expected.size());
		for (idx_t row = 0; row < rows.size(); row++) {
			INFO("row " << row);
			REQUIRE(rows[row].ends == expected[row].ends);
		}
	}
}

TEST_CASE("Rows of a live stream read in ranges by many threads match the rows of the whole input", "[csv]") {
	std::mt19937_64 rng(47);
	for (idx_t round = 0; round < 12; round++) {
		const bool text = round % 2;
		const auto dialect = text ? RowDialect::PostgresText('\t') : RowDialect::PostgresCsv(',', '"', '"');
		string input;
		for (idx_t row = 0; row < 3000 + rng() % 3000; row++) {
			input += LineitemRow(rng, text ? '\t' : ',');
		}
		RowLexer lexer(dialect, 16, NumericLimits<idx_t>::Maximum(), true, false, true);
		RowRangesTestStream whole(input);
		auto expected = ReadRows(lexer, whole);

		LiveRowStream live;
		RowRanges ranges(Allocator::DefaultAllocator(), lexer, live, 0, NumericLimits<idx_t>::Maximum(),
		                 1024 << (rng() % 4), true);
		absl::Mutex rows_lock;
		vector<LexedRow> rows;
		vector<std::thread> readers;
		for (idx_t thread = 0; thread < 8; thread++) {
			readers.emplace_back([&]() {
				RowRangeReader reader(ranges);
				vector<LexedRow> mine;
				idx_t range = 0;
				while (ranges.Claim(range)) {
					if (!reader.StartIntake(range, true, true, false)) {
						continue;
					}
					while (true) {
						const auto result = reader.Advance(STANDARD_VECTOR_SIZE);
						if (result == RowReadResult::DONE) {
							break;
						}
						TakeRows(reader, mine);
					}
					ranges.Finish(range);
				}
				const absl::MutexLock guard(&rows_lock);
				rows.insert(rows.end(), mine.begin(), mine.end());
			});
		}
		std::mt19937_64 writer_rng(round);
		for (idx_t at = 0; at < input.size();) {
			const idx_t piece = MinValue<idx_t>(1 + writer_rng() % 5000, input.size() - at);
			live.Append(std::string_view(input).substr(at, piece));
			at += piece;
			std::this_thread::sleep_for(std::chrono::microseconds(writer_rng() % 300));
		}
		live.Close();
		for (auto &reader : readers) {
			reader.join();
		}
		const auto by_end = [](const LexedRow &left, const LexedRow &right) {
			return left.ends.back() < right.ends.back();
		};
		std::sort(rows.begin(), rows.end(), by_end);
		std::sort(expected.begin(), expected.end(), by_end);
		INFO("round " << round);
		REQUIRE(!live.ReadReleased());
		REQUIRE(rows.size() == expected.size());
		for (idx_t row = 0; row < rows.size(); row++) {
			INFO("row " << row);
			REQUIRE(rows[row].ends == expected[row].ends);
		}
	}
}

TEST_CASE("Rows of a live stream read piece by piece match the rows of the whole input", "[csv]") {
	const vector<RowDialect> dialects = {RowDialect::PostgresText('\t'), RowDialect::PostgresCsv(',', '"', '"'),
	                                     RowDialect::PostgresCsv(',', '"', '\\'),
	                                     RowDialect::DuckDB(TestGrammar(',', false, NewLineIdentifier::CARRY_ON)),
	                                     RowDialect::DuckDB(TestGrammar(',', true, NewLineIdentifier::SINGLE_N))};
	std::mt19937_64 rng(41);
	for (idx_t round = 0; round < 600; round++) {
		const auto &dialect = dialects[round % dialects.size()];
		const idx_t columns = 1 + rng() % 4;
		string input;
		const idx_t row_count = rng() % 40;
		for (idx_t row = 0; row < row_count; row++) {
			for (idx_t column = 0; column < columns; column++) {
				if (column > 0) {
					input += dialect.delimiter;
				}
				input += RandomField(rng, dialect);
			}
			input += dialect.duckdb && rng() % 2 ? "\r\n" : "\n";
		}
		if (!input.empty() && rng() % 3 == 0) {
			input.resize(input.size() - 1);
		}
		RowLexer lexer(dialect, columns, NumericLimits<idx_t>::Maximum(), true, false, true);
		RowRangesTestStream whole(input);
		const auto expected = ReadRows(lexer, whole);

		LiveRowStream live;
		vector<LexedRow> rows;
		const bool pieces = (round / dialects.size()) % 2;
		std::thread reader_thread(
		    [&]() { rows = pieces ? ReadRanges(lexer, live, idx_t(64) << (round % 3)) : ReadRows(lexer, live); });
		for (idx_t at = 0; at < input.size();) {
			const idx_t piece = MinValue<idx_t>(1 + rng() % 150, input.size() - at);
			live.AwaitReader();
			live.Append(std::string_view(input).substr(at, piece));
			at += piece;
		}
		live.AwaitReader();
		live.Close();
		reader_thread.join();
		INFO("round " << round << " input " << input);
		if (!pieces) {
			REQUIRE(rows == expected);
			continue;
		}
		REQUIRE(!live.ReadReleased());
		REQUIRE(rows.size() == expected.size());
		for (idx_t row = 0; row < rows.size(); row++) {
			INFO("row " << row);
			REQUIRE(rows[row].ends == expected[row].ends);
		}
	}
}

TEST_CASE("Row sync finds the first row after a position in text and csv input", "[csv]") {
	const auto text = RowDialect::PostgresText('\t');
	string data = "a\n\r\nb";
	REQUIRE(RowSync(text, data.data(), 0, 0, data.size(), true) == 4);
	data = "a\n\r";
	REQUIRE(RowSync(text, data.data(), 0, 0, data.size(), false) == DConstants::INVALID_INDEX);
	REQUIRE(RowSync(text, data.data(), 0, 0, data.size(), true) == 2);
	data = "a\n\rb";
	REQUIRE(RowSync(text, data.data(), 0, 0, data.size(), true) == 2);

	const auto text_carriage = RowDialect::PostgresText('\t', '\r');
	data = "a\r\rb\nc";
	REQUIRE(RowSync(text_carriage, data.data(), 0, 0, data.size(), true) == 3);

	const auto csv = RowDialect::PostgresCsv(',', '"', '\\');
	data = "a\r";
	REQUIRE(RowSync(csv, data.data(), 0, 0, data.size(), false) == DConstants::INVALID_INDEX);
	REQUIRE(RowSync(csv, data.data(), 0, 0, data.size(), true) == DConstants::INVALID_INDEX);
	const auto csv_carriage = RowDialect::PostgresCsv(',', '"', '\\', '\r');
	REQUIRE(RowSync(csv_carriage, data.data(), 0, 0, data.size(), true) == 2);
	data = "a\r\nb\rc";
	REQUIRE(RowSync(csv_carriage, data.data(), 0, 0, data.size(), true) == 2);
	data = "\nab";
	REQUIRE(RowSync(csv, data.data(), 0, 0, data.size(), true) == 1);
	data = "\\\r\nx\ny";
	REQUIRE(RowSync(csv, data.data(), 0, 0, data.size(), true) == 5);
	data = "a\nb\r\"c";
	REQUIRE(RowSync(csv, data.data(), 0, 0, data.size(), true) == 2);
}

TEST_CASE("Row sync finds the first line break like a byte-by-byte scan", "[csv]") {
	CsvGrammar plain;
	plain.delimiter[0] = ',';
	CsvGrammar escaped = plain;
	escaped.escape = '\\';
	const RowDialect dialects[] = {RowDialect::PostgresCsv(',', '\0', '\0'), RowDialect::PostgresCsv(',', '\0', '\\'),
	                               RowDialect::PostgresCsv(',', '\0', '\\', '\r'), RowDialect::DuckDB(plain),
	                               RowDialect::DuckDB(escaped)};
	const char breaks[] = {'\n', '\r', '\\'};
	std::mt19937_64 rng(42);
	for (idx_t round = 0; round < 20000; round++) {
		const auto &dialect = dialects[round % 5];
		const idx_t sparsity = idx_t(1) << (rng() % 10);
		string data;
		const idx_t size = rng() % 300;
		for (idx_t i = 0; i < size; i++) {
			data += rng() % sparsity == 0 ? breaks[rng() % 3] : 'a';
		}
		const idx_t base = rng() % 100;
		const idx_t begin = base + rng() % (size + 1);
		const idx_t end = begin + rng() % (base + size - begin + 1);
		const bool eof = rng() % 2;
		REQUIRE(RowSync(dialect, data.data(), base, begin, end, eof) ==
		        ScanLineBreak(dialect, data, base, begin, end, eof));
	}
}

TEST_CASE("Row ranges of a stream find its end from what its source reads", "[csv]") {
	const string data = "a,b\nc,d\n";
	RowLexer lexer(RowDialect::PostgresCsv(',', '"', '"'), 2, 1 << 20, true);

	PlainRowStream plain(data);
	auto &allocator = Allocator::DefaultAllocator();
	RowRanges unknown_end(allocator, lexer, plain, 0, NumericLimits<idx_t>::Maximum(), DConstants::INVALID_INDEX, true);
	RowBytes bytes;
	unknown_end.ReadInto(bytes, 0, 100);
	REQUIRE(bytes.size == data.size());
	REQUIRE(unknown_end.Size() == data.size());
	unknown_end.ReadInto(bytes, data.size(), 10);
	REQUIRE(bytes.size == 0);

	RowRanges beyond(allocator, lexer, plain, 0, NumericLimits<idx_t>::Maximum(), DConstants::INVALID_INDEX, true);
	RowBytes empty;
	beyond.ReadInto(empty, 100, 10);
	REQUIRE(empty.size == 0);
	REQUIRE(beyond.Size() == NumericLimits<idx_t>::Maximum());

	RowRangesTestStream known(data);
	RowRanges known_end(allocator, lexer, known, 0, NumericLimits<idx_t>::Maximum(), DConstants::INVALID_INDEX, true);
	RowBytes known_bytes;
	known_end.ReadInto(known_bytes, 4, 100);
	REQUIRE(known_bytes.size == 4);
	REQUIRE(known_end.Size() == data.size());
}

TEST_CASE("A range reader reads and counts the rows of a stream", "[csv]") {
	const string data = "a,b\nc,d\n";
	RowLexer lexer(RowDialect::PostgresCsv(',', '"', '"'), 2, 1 << 20, true);
	PlainRowStream source(data);

	RowRanges ranges(Allocator::DefaultAllocator(), lexer, source, 0, NumericLimits<idx_t>::Maximum(),
	                 DConstants::INVALID_INDEX, true);
	idx_t range = DConstants::INVALID_INDEX;
	REQUIRE(ranges.Claim(range));
	RowRangeReader reader(ranges);
	reader.Start(range, true, true, true);
	REQUIRE(reader.Active());
	REQUIRE(reader.Range() == 0);
	idx_t rows = 0;
	while (true) {
		const auto result = reader.Advance(STANDARD_VECTOR_SIZE);
		if (result == RowReadResult::DONE) {
			break;
		}
		REQUIRE(result == RowReadResult::ROWS);
		const idx_t ready = reader.Ready();
		rows += ready;
		reader.Emit(ready);
	}
	REQUIRE(rows == 2);
	REQUIRE(!reader.Active());
	REQUIRE(reader.Finished());
	REQUIRE(reader.Stop() == data.size());

	RowRanges counted(Allocator::DefaultAllocator(), lexer, source, 0, NumericLimits<idx_t>::Maximum(),
	                  DConstants::INVALID_INDEX, true);
	REQUIRE(counted.Claim(range));
	RowRangeReader counter(counted);
	counter.Start(range, false, false, false);
	REQUIRE(counter.Advance(STANDARD_VECTOR_SIZE) == RowReadResult::COUNTED);
	REQUIRE(counter.CountedRows() == 2);
}

TEST_CASE("Range origins, transfers and syncs at the edges of the input", "[csv]") {
	const string data = "x\",y\nz,w\n";
	RowLexer lexer(RowDialect::PostgresCsv(',', '"', '"'), 2, 1 << 20, true);
	RowRangesTestStream source(data);
	RowRanges ranges(Allocator::DefaultAllocator(), lexer, source, 0, data.size(), DConstants::INVALID_INDEX, true);
	RowBytes bytes;
	ranges.ReadInto(bytes, 0, data.size());
	REQUIRE(ranges.Origin(bytes, 0) == CSVState::NOT_SET);
	REQUIRE(ranges.Origin(bytes, 5) == CSVState::RECORD_SEPARATOR);
	const auto quoted = ranges.LexTransfer(bytes, 0, data.size(), CSVState::QUOTED_NEW_LINE);
	REQUIRE(quoted.computed);
	REQUIRE(quoted.first_boundary == 5);
	REQUIRE(quoted.end.states[1] == CSVState::RECORD_SEPARATOR);

	const string crlf = "\nz,w\r\n";
	RowLexer crlf_lexer(RowDialect::DuckDB(TestGrammar(',', true, NewLineIdentifier::CARRY_ON)), 2, 1 << 20, true);
	RowRangesTestStream crlf_source(crlf);
	RowRanges crlf_ranges(Allocator::DefaultAllocator(), crlf_lexer, crlf_source, 0, crlf.size(),
	                      DConstants::INVALID_INDEX, true);
	RowBytes crlf_bytes;
	crlf_ranges.ReadInto(crlf_bytes, 0, crlf.size());
	const auto after_cr = crlf_ranges.LexTransfer(crlf_bytes, 0, crlf.size(), CSVState::CARRIAGE_RETURN);
	REQUIRE(after_cr.computed);
	REQUIRE(after_cr.end.states[1] == CSVState::RECORD_SEPARATOR);

	const string unterminated = "abc";
	RowRangesTestStream unterminated_source(unterminated);
	RowRanges unterminated_ranges(Allocator::DefaultAllocator(), lexer, unterminated_source, 0, unterminated.size(),
	                              DConstants::INVALID_INDEX, true);
	REQUIRE(unterminated_ranges.SyncAt(0) == unterminated.size());
}

TEST_CASE("Row fields keep their events, anomalies and extras when dropped and rebased", "[csv]") {
	RowFields fields;
	fields.Reserve(16);
	for (uint32_t i = 0; i < 4; i++) {
		fields.rows[i] = 100 + i * 10;
	}
	fields.row_count = 4;
	for (uint32_t i = 0; i < 8; i++) {
		fields.ends[i] = 100 + i * 5;
	}
	fields.end_count = 8;
	fields.events.push_back({0, 0, 100});
	fields.events.push_back({3, 1, 140});
	fields.anomalies.push_back({1, 3, 120, 0});
	fields.anomalies.push_back({3, 1, 140, 0});
	fields.extras.push_back(130 | ROW_FIELD_FLAG);
	fields.Rebase(100);
	REQUIRE(fields.rows[0] == 0);
	REQUIRE(fields.ends[7] == 35);
	REQUIRE(fields.extras[0] == (30 | ROW_FIELD_FLAG));
	REQUIRE(fields.anomalies[0].end == 20);
	REQUIRE(fields.events[1].position == 40);
	fields.Drop(2, 4);
	REQUIRE(fields.row_count == 2);
	REQUIRE(fields.end_count == 4);
	REQUIRE(fields.events.size() - fields.event_head == 1);
	REQUIRE(fields.events[fields.event_head].row - fields.base == 1);
	REQUIRE(fields.HasAnomalies());
	REQUIRE(!fields.Anomaly(0));
	REQUIRE(fields.Anomaly(1)->fields == 1);
	REQUIRE(fields.extras.size() == 1);
	fields.Drop(2, 4);
	REQUIRE(!fields.HasAnomalies());
	REQUIRE(fields.events.empty());
	REQUIRE(fields.extras.empty());
}

TEST_CASE("Row fields count the rows they drop without rewriting what they still hold", "[csv]") {
	constexpr idx_t ROWS = (idx_t(1) << 20) + 4096;
	RowFields fields;
	fields.Reserve(ROWS);
	for (idx_t i = 0; i < ROWS; i++) {
		fields.rows[i] = static_cast<uint32_t>(i);
		fields.ends[i] = static_cast<uint32_t>(i);
	}
	fields.row_count = ROWS;
	fields.end_count = ROWS;
	for (uint32_t row = 0; row < ROWS; row += 2) {
		fields.anomalies.push_back({row, 0, row, 0});
		fields.events.push_back({row + 1, 0, row + 1});
	}
	for (idx_t dropped = 0; dropped + 2048 <= ROWS - 4096; dropped += 2048) {
		REQUIRE(fields.Anomaly(0));
		REQUIRE(!fields.Anomaly(1));
		REQUIRE(fields.Anomaly(0)->end == dropped);
		REQUIRE(fields.events[fields.event_head].row - fields.base == 1);
		fields.Drop(2048, 2048);
	}
	REQUIRE(fields.base < (idx_t(1) << 20));
	REQUIRE(fields.rows[0] == (idx_t(1) << 20));
	REQUIRE(fields.Anomaly(2)->end == (idx_t(1) << 20) + 2);
	REQUIRE(fields.events[fields.event_head].row - fields.base == 1);
	REQUIRE(fields.events[fields.event_head].position == (idx_t(1) << 20) + 1);
}

TEST_CASE("Row skip waits for the line feed of a carriage return at the end of its input", "[csv]") {
	const auto dialect = RowDialect::DuckDB(TestGrammar(',', false, NewLineIdentifier::CARRY_ON));
	RowSkip none(dialect, 0, 5);
	REQUIRE(none.Start() == 5);
	REQUIRE(none.Feed(nullptr, 5, 5, true));

	RowSkip skip(dialect, 1);
	const string first = string(63, 'a') + "\r";
	REQUIRE(!skip.Feed(first.data(), 0, first.size(), false));
	const string second = "\nb\n";
	REQUIRE(skip.Feed(second.data(), first.size(), first.size() + second.size(), true));
	REQUIRE(skip.Start() == first.size() + 1);
	REQUIRE(skip.Feed(nullptr, 0, 0, true));
}

TEST_CASE("Rows with too few fields are padded when the lexer pads rows", "[csv]") {
	const auto dialect = RowDialect::DuckDB(TestGrammar(',', true, NewLineIdentifier::NOT_SET));
	RowLexer lexer(dialect, 10, 1 << 20, true, true, false);

	string sparse;
	for (idx_t row = 0; row < 64; row++) {
		sparse += "1\n";
	}
	RowCursor cursor;
	RowFields fields;
	lexer.Start(cursor, 0, true);
	REQUIRE(lexer.Feed(cursor, sparse.data(), 0, sparse.size(), true, &fields));
	CsvRangeEntry entry;
	REQUIRE(lexer.Finish(cursor, sparse.data(), 0, sparse.size(), true, &fields, CSVState::NOT_SET, entry));
	REQUIRE(cursor.padded);
	REQUIRE(fields.row_count == 64);
	REQUIRE(fields.end_count == 640);
	REQUIRE(fields.ends[0] == 1);
	REQUIRE(fields.ends[1] == (1 | ROW_FIELD_FLAG));
	REQUIRE(fields.ends[9] == (1 | ROW_FIELD_FLAG));
	REQUIRE(fields.ends[10] == 3);
	REQUIRE(fields.ends[639] == (127 | ROW_FIELD_FLAG));

	string short_by_one;
	for (idx_t row = 0; row < 8; row++) {
		short_by_one += "1,2,3,4,5,6,7,8,9\n";
	}
	RowCursor reserved_cursor;
	RowFields reserved;
	reserved.Reserve(4096);
	lexer.Start(reserved_cursor, 0, true);
	REQUIRE(lexer.Feed(reserved_cursor, short_by_one.data(), 0, short_by_one.size(), true, &reserved));
	REQUIRE(reserved.row_count == 8);
	REQUIRE(reserved.end_count == 80);
	REQUIRE(reserved.ends[8] == 17);
	REQUIRE(reserved.ends[9] == (17 | ROW_FIELD_FLAG));
	REQUIRE(reserved.ends[79] == (143 | ROW_FIELD_FLAG));

	RowLexer three(dialect, 3, 1 << 20, true, true, false);
	const string tail = "1,2,3\n4";
	RowCursor tail_cursor;
	RowFields tail_fields;
	three.Start(tail_cursor, 0, true);
	REQUIRE(three.Feed(tail_cursor, tail.data(), 0, tail.size(), true, &tail_fields));
	REQUIRE(three.Finish(tail_cursor, tail.data(), 0, tail.size(), true, &tail_fields, CSVState::NOT_SET, entry));
	REQUIRE(tail_cursor.padded);
	REQUIRE(tail_fields.end_count == 6);
	REQUIRE(tail_fields.ends[3] == 7);
	REQUIRE(tail_fields.ends[4] == (7 | ROW_FIELD_FLAG));
	REQUIRE(tail_fields.ends[5] == (7 | ROW_FIELD_FLAG));
}

TEST_CASE("The row lexer refuses input it did not reach and a lone carriage return in strict counting", "[csv]") {
	const auto dialect = RowDialect::DuckDB(TestGrammar(',', true, NewLineIdentifier::NOT_SET));
	RowLexer lexer(dialect, 3, 1 << 20, true);
	const string data = "1,2,3\n";
	RowCursor cursor;
	RowFields fields;
	lexer.Start(cursor, 0, true);
	REQUIRE(lexer.Feed(cursor, data.data(), 0, 3, false, &fields));
	CsvRangeEntry entry;
	REQUIRE(!lexer.Finish(cursor, data.data(), 0, data.size(), true, &fields, CSVState::NOT_SET, entry));
	REQUIRE(!lexer.Feed(cursor, data.data(), 0, data.size(), true, &fields));

	for (const char delimiter : {',', ' '}) {
		const string lone = string("a") + delimiter + "c\rb" + delimiter + "d\n";
		RowLexer counter(RowDialect::DuckDB(TestGrammar(delimiter, true, NewLineIdentifier::CARRY_ON)), 2, 1 << 20,
		                 true);
		RowCursor counting;
		counter.Start(counting, 0, true);
		REQUIRE(!counter.Feed(counting, lone.data(), 0, lone.size(), true, nullptr));
		RowCursor extracting;
		RowFields lone_fields;
		counter.Start(extracting, 0, true);
		REQUIRE(counter.Feed(extracting, lone.data(), 0, lone.size(), true, &lone_fields));
		REQUIRE(!lone_fields.events.empty());
	}
}

TEST_CASE("The lean step reads quoted rows without trimmed spaces and with every line break a row end", "[csv]") {
	const auto spaced = RowDialect::DuckDB(TestGrammar(' ', true, NewLineIdentifier::NOT_SET));
	REQUIRE(!spaced.quoted_spaces);
	RowLexer lexer(spaced, 2, 1 << 20, true);
	string quoted;
	for (idx_t row = 0; row < 11; row++) {
		quoted += "\"a\" b\n";
	}
	RowCursor cursor;
	RowFields fields;
	lexer.Start(cursor, 0, false);
	REQUIRE(lexer.Feed(cursor, quoted.data(), 0, quoted.size(), true, &fields));
	CsvRangeEntry entry;
	REQUIRE(lexer.Finish(cursor, quoted.data(), 0, quoted.size(), true, &fields, CSVState::NOT_SET, entry));
	REQUIRE(fields.row_count == 11);
	REQUIRE(cursor.settled);

	auto unmerged = RowDialect::DuckDB(TestGrammar(',', true, NewLineIdentifier::NOT_SET));
	unmerged.merge_terms = false;
	RowLexer plain(unmerged, 2, 1 << 20, true);
	string rows;
	for (idx_t row = 0; row < 16; row++) {
		rows += "a,b\n";
	}
	RowCursor plain_cursor;
	RowFields plain_fields;
	plain.Start(plain_cursor, 0, true);
	REQUIRE(plain.Feed(plain_cursor, rows.data(), 0, rows.size(), true, &plain_fields));
	REQUIRE(plain_fields.row_count == 16);
	REQUIRE(plain_fields.end_count == 32);

	const auto unescaped = RowDialect::PostgresCsv(',', '"', '\0');
	REQUIRE(!unescaped.escape_in_quotes);
	RowLexer postgres(unescaped, 2, 1 << 20, true);
	const string quoted_field = "a,\"b\"\n";
	RowCursor postgres_cursor;
	RowFields postgres_fields;
	postgres.Start(postgres_cursor, 0, true);
	REQUIRE(postgres.Feed(postgres_cursor, quoted_field.data(), 0, quoted_field.size(), true, &postgres_fields));
	REQUIRE(postgres_fields.row_count == 1);
	REQUIRE(postgres_fields.ends[0] == 1);
	REQUIRE(postgres_fields.ends[1] == (5 | ROW_FIELD_FLAG));
}

TEST_CASE("read_csv reads a live host stream in parallel as it arrives", "[csv]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads = 16"));
	std::mt19937_64 rng(53);
	string data;
	idx_t sum = 0;
	for (idx_t row = 0; row < 200000; row++) {
		const idx_t value = rng() % 1000000;
		sum += value;
		data += to_string(value) + ",\"" + string(rng() % 40, 'x') + ",y\"\n";
	}
	const auto path = TestCreatePath("live_row_stream.csv");
	{
		std::ofstream out(path, std::ios::binary);
		out << "0,a\n";
	}
	for (idx_t round = 0; round < 4; round++) {
		auto live = make_shared_ptr<LiveRowStream>();
		con.context->registered_state->Insert(RowStreamKey(path), make_shared_ptr<RowStreamState>(live));
		std::thread writer([&]() {
			std::mt19937_64 writer_rng(round);
			for (idx_t at = 0; at < data.size();) {
				const idx_t piece = MinValue<idx_t>(1 + writer_rng() % 200000, data.size() - at);
				live->Append(std::string_view(data).substr(at, piece));
				at += piece;
				std::this_thread::sleep_for(std::chrono::microseconds(writer_rng() % 500));
			}
			live->Close();
		});
		auto result = con.Query("SELECT count(*), sum(a), count(DISTINCT b) FROM read_csv('" + path +
		                        "', auto_detect = false, header = false, columns = {'a': 'BIGINT', 'b': 'VARCHAR'})");
		writer.join();
		con.context->registered_state->Remove(RowStreamKey(path));
		INFO("round " << round);
		REQUIRE(CHECK_COLUMN(result, 0, {200000}));
		REQUIRE(CHECK_COLUMN(result, 1, {Value::BIGINT(NumericCast<int64_t>(sum))}));
		REQUIRE(CHECK_COLUMN(result, 2, {40}));
		REQUIRE(!live->ReadReleased());
	}
}

TEST_CASE("read_json reads a live host stream as it arrives, however it is split", "[csv]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads = 8"));
	std::mt19937_64 rng(71);
	string lines;
	string array = "\xEF\xBB\xBF \n[ \n";
	idx_t sum = 0;
	for (idx_t row = 0; row < 30000; row++) {
		const idx_t value = rng() % 1000000;
		sum += value;
		const string record = "{\"a\": " + to_string(value) + ", \"b\": \"" + string(rng() % 40, 'x') + "\"}";
		lines += record + "\n";
		array += (row ? " ,\n " : "") + record;
	}
	array += " \n]\n";
	const auto path = TestCreatePath("live_row_stream.json");
	{
		std::ofstream out(path, std::ios::binary);
		out << "{}\n";
	}
	const vector<std::pair<string, string>> inputs = {{lines, "newline_delimited"}, {array, "array"}};
	for (auto &[data, format] : inputs) {
		for (idx_t round = 0; round < 3; round++) {
			auto live = make_shared_ptr<LiveRowStream>();
			con.context->registered_state->Insert(RowStreamKey(path), make_shared_ptr<RowStreamState>(live));
			std::thread writer([&]() {
				std::mt19937_64 writer_rng(round);
				const idx_t largest = round == 0 ? 3 : 100000;
				for (idx_t at = 0; at < data.size();) {
					const idx_t piece = MinValue<idx_t>(1 + writer_rng() % largest, data.size() - at);
					live->Append(std::string_view(data).substr(at, piece));
					at += piece;
					if (writer_rng() % 64 == 0) {
						std::this_thread::sleep_for(std::chrono::microseconds(writer_rng() % 200));
					}
				}
				live->Close();
			});
			auto result = con.Query("SELECT count(*), sum(a), count(DISTINCT b) FROM read_json('" + path +
			                        "', format = '" + format + "', columns = {'a': 'BIGINT', 'b': 'VARCHAR'})");
			writer.join();
			con.context->registered_state->Remove(RowStreamKey(path));
			INFO(format << " round " << round);
			REQUIRE(CHECK_COLUMN(result, 0, {30000}));
			REQUIRE(CHECK_COLUMN(result, 1, {Value::BIGINT(NumericCast<int64_t>(sum))}));
			REQUIRE(CHECK_COLUMN(result, 2, {40}));
			REQUIRE(!live->ReadReleased());
		}
	}
}

TEST_CASE("read_csv reads the stream the host registers for its path", "[csv]") {
	DuckDB db(nullptr);
	Connection con(db);
	string data;
	for (idx_t row = 0; row < 1000; row++) {
		data += to_string(row) + "," + to_string(row * 2) + "\n";
	}
	const auto path = TestCreatePath("host_row_stream.csv");
	{
		std::ofstream out(path, std::ios::binary);
		out << "0,0\n";
	}
	con.context->registered_state->Insert(RowStreamKey(path),
	                                      make_shared_ptr<RowStreamState>(make_shared_ptr<PlainRowStream>(data)));
	const string read = "read_csv('" + path +
	                    "', auto_detect = false, header = false, columns = {'a': 'BIGINT', 'b': 'BIGINT'}, "
	                    "buffer_size = 1024)";
	auto result = con.Query("SELECT count(*), sum(a), sum(b) FROM " + read);
	REQUIRE(CHECK_COLUMN(result, 0, {1000}));
	REQUIRE(CHECK_COLUMN(result, 1, {499500}));
	REQUIRE(CHECK_COLUMN(result, 2, {999000}));
	result = con.Query("SELECT count(*) FROM " + read);
	REQUIRE(CHECK_COLUMN(result, 0, {1000}));
}

TEST_CASE("The window that holds a row of a host stream counts against memory_limit", "[csv]") {
	DuckDB db(nullptr);
	Connection con(db);
	string data;
	for (idx_t row = 0; row < 200000; row++) {
		data += row == 1000 ? string(20000000, 'x') : "y" + to_string(row);
		data += "\n";
	}
	const auto path = TestCreatePath("host_row_stream_big_row.csv");
	{
		std::ofstream out(path, std::ios::binary);
		out << "y\n";
	}
	con.context->registered_state->Insert(RowStreamKey(path),
	                                      make_shared_ptr<RowStreamState>(make_shared_ptr<PlainRowStream>(data)));
	const string query = "SELECT max(length(s)) FROM read_csv('" + path +
	                     "', columns = {'s': 'VARCHAR'}, header = false, auto_detect = false, new_line = '\\n', "
	                     "max_line_size = 32000000, buffer_size = 32000000)";
	REQUIRE_NO_FAIL(con.Query("SET threads = 1"));
	REQUIRE_NO_FAIL(con.Query("SET memory_limit = '10MiB'"));
	auto result = con.Query(query);
	REQUIRE(result->HasError());
	REQUIRE(result->GetErrorType() == ExceptionType::OUT_OF_MEMORY);
	REQUIRE_NO_FAIL(con.Query("RESET memory_limit"));
	result = con.Query(query);
	REQUIRE(CHECK_COLUMN(result, 0, {20000000}));
}

TEST_CASE("A range waiting on the chain computes the transfers no reader published", "[csv]") {
	string data;
	for (idx_t row = 0; row < 40; row++) {
		data += to_string(row) + ",\"text with , and\nlines and \"\"quotes\"\" " + string(row % 7, 'x') + "\"\n";
	}
	const RowDialect dialects[] = {RowDialect::PostgresCsv(',', '"', '"'),
	                               RowDialect::DuckDB(TestGrammar(',', false, NewLineIdentifier::SINGLE_N))};
	for (auto &dialect : dialects) {
		RowLexer lexer(dialect, 2, NumericLimits<idx_t>::Maximum(), true);
		RowRangesTestStream source(data);
		RowRanges lazy(Allocator::DefaultAllocator(), lexer, source, 0, data.size(), 13, true);
		RowRanges published(Allocator::DefaultAllocator(), lexer, source, 0, data.size(), 13, true);
		const idx_t count = lazy.RangeCount();
		REQUIRE(count > 100);
		lazy.StartState(count - 1);
		for (idx_t range = 0; range < count; range++) {
			const auto start = published.StartState(range);
			published.Publish(range, start.states[1], published.Transfer(range, start.states[1]));
			const auto resolved = lazy.StartState(range);
			REQUIRE(resolved.states[0] == start.states[0]);
			REQUIRE(resolved.states[1] == start.states[1]);
			REQUIRE(resolved.line_end == start.line_end);
			REQUIRE(resolved.last_position == start.last_position);
		}
	}
}

TEST_CASE("Ranges inside a row longer than many ranges read only their own bytes", "[csv]") {
	const string field(idx_t(1) << 20, 'a');
	string rest;
	for (idx_t row = 0; row < 100; row++) {
		rest += "r" + to_string(row) + "," + to_string(row) + "\n";
	}
	const vector<std::pair<RowDialect, string>> cases = {
	    {RowDialect::PostgresCsv(',', '"', '"'), "\"" + field + "\",1\n" + rest},
	    {RowDialect::DuckDB(TestGrammar(',', true, NewLineIdentifier::SINGLE_N)), "\"" + field + "\",1\n" + rest},
	    {RowDialect::DuckDB(TestGrammar(',', false, NewLineIdentifier::CARRY_ON)), field + ",1\r\n" + rest},
	    {RowDialect::PostgresText(','), field + ",1\n" + rest}};
	const CSVState origins[] = {CSVState::NOT_SET, CSVState::RECORD_SEPARATOR, CSVState::CARRIAGE_RETURN,
	                            CSVState::QUOTED,  CSVState::QUOTED_NEW_LINE,  CSVState::STANDARD};
	for (auto &[dialect, data] : cases) {
		RowLexer lexer(dialect, 2, NumericLimits<idx_t>::Maximum(), true);
		CountingRowSource source(data);
		RowRanges ranges(Allocator::DefaultAllocator(), lexer, source, 0, data.size(), RowWindow::PIECE, true);
		const idx_t count = ranges.RangeCount();
		REQUIRE(count > 16);
		RowRangeReader reader(ranges);
		vector<LexedRow> rows;
		idx_t range = 0;
		while (ranges.Claim(range)) {
			reader.Start(range, true, false, false);
			while (true) {
				const auto result = reader.Advance(STANDARD_VECTOR_SIZE);
				if (result == RowReadResult::DONE) {
					break;
				}
				REQUIRE(result == RowReadResult::ROWS);
				TakeRows(reader, rows);
			}
			ranges.Finish(range);
		}
		REQUIRE(source.read < 3 * data.size());
		RowRangesTestStream whole(data);
		REQUIRE(rows.size() == 101);
		REQUIRE(rows == ReadRows(lexer, whole));

		RowBytes all;
		ranges.ReadInto(all, 0, data.size());
		const auto same = [](const CsvRangeEntry &left, const CsvRangeEntry &right) {
			const auto same_start = [](const CsvRangeStart &a, const CsvRangeStart &b) {
				return a.states[0] == b.states[0] && a.states[1] == b.states[1] && a.line_end == b.line_end &&
				       a.last_position == b.last_position;
			};
			return left.computed == right.computed && left.first_boundary == right.first_boundary &&
			       same_start(left.boundary, right.boundary) && same_start(left.end, right.end);
		};
		for (range = 0; range < count; range++) {
			const idx_t begin = range == 0 ? 0 : ranges.SyncAt(ranges.RangeBegin(range) - 1);
			const idx_t end =
			    range + 1 == count ? data.size() : MaxValue(ranges.SyncAt(ranges.RangeEnd(range) - 1), begin);
			for (const auto origin : origins) {
				const auto expected = ranges.LexTransfer(all, begin, end, origin);
				REQUIRE(same(ranges.Transfer(range, origin), expected));
				if (begin == end) {
					REQUIRE(same(ranges.Entry(range, origin), expected));
				}
			}
		}
	}
}

TEST_CASE("A single-threaded scan of a compressed CSV holds only the buffers it still reads", "[csv]") {
	DuckDB db(nullptr);
	Connection con(db);
	const auto path = TestCreatePath("release_buffers.csv.gz");
	REQUIRE_NO_FAIL(con.Query("COPY (SELECT i AS a, 'value number ' || i AS b FROM range(1000000) t(i)) TO '" + path +
	                          "' (HEADER false)"));
	REQUIRE_NO_FAIL(con.Query("SET threads = 1"));
	auto stream =
	    OpenStream(con, "SELECT a, b FROM read_csv('" + path +
	                        "', parallel = false, buffer_size = 1048576, header = false, auto_detect = false, "
	                        "columns = {'a': 'BIGINT', 'b': 'VARCHAR'})");
	auto &buffer_manager = BufferManager::GetBufferManager(*con.context);
	idx_t rows = 0;
	idx_t held = 0;
	while (auto chunk = stream->Fetch()) {
		rows += chunk->size();
		for (auto &info : buffer_manager.GetMemoryUsageInfo()) {
			if (info.tag == MemoryTag::CSV_READER) {
				held = MaxValue(held, info.size);
			}
		}
	}
	REQUIRE(rows == 1000000);
	REQUIRE(held <= 8 * 1048576);
}

TEST_CASE("A sequential CSV buffer manager releases a buffer handed back before the one ahead of it", "[csv]") {
	DuckDB db(nullptr);
	Connection con(db);
	const auto path = TestCreatePath("release_out_of_order.csv.gz");
	REQUIRE_NO_FAIL(con.Query("COPY (SELECT i AS a, 'value number ' || i AS b FROM range(1000000) t(i)) TO '" + path +
	                          "' (HEADER false)"));
	CSVReaderOptions options;
	options.buffer_size_option.Set(1048576);
	auto manager = CSVBufferManager::Open(*con.context, options, OpenFileInfo(path), false, nullptr);
	REQUIRE(!manager->HasKnownBufferRanges());
	auto &buffer_manager = BufferManager::GetBufferManager(*con.context);
	const auto held = [&]() {
		idx_t size = 0;
		for (auto &info : buffer_manager.GetMemoryUsageInfo()) {
			if (info.tag == MemoryTag::CSV_READER) {
				size = info.size;
			}
		}
		return size;
	};
	for (idx_t buffer = 0; buffer < 3; buffer++) {
		REQUIRE(manager->GetBuffer(buffer));
	}
	const idx_t loaded = held();
	manager->ResetBuffer(1);
	REQUIRE(held() == loaded);
	manager->ResetBuffer(0);
	REQUIRE(held() + 1048576 <= loaded);
}

class FailingReadFileSystem : public LocalFileSystem {
public:
	explicit FailingReadFileSystem(string path_p) : path(std::move(path_p)) {
	}

	string GetName() const override {
		return "FailingReadFileSystem";
	}
	bool CanHandleFile(const string &fpath) override {
		return fpath == path;
	}
	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		if (fail.exchange(false)) {
			throw IOException("Injected read failure");
		}
		LocalFileSystem::Read(handle, buffer, nr_bytes, location);
	}

	const string path;
	std::atomic<bool> fail {false};
};

TEST_CASE("A CSV buffer whose load failed is loaded again by the next pin", "[csv]") {
	DuckDB db(nullptr);
	Connection con(db);
	const auto path = TestCreatePath("load_failure.csv");
	REQUIRE_NO_FAIL(con.Query("COPY (SELECT i AS a, 'value number ' || i AS b FROM range(50000) t(i)) TO '" + path +
	                          "' (HEADER false)"));
	std::ifstream in(path, std::ios::binary);
	const string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	auto failing = make_uniq<FailingReadFileSystem>(path);
	auto &fail = failing->fail;
	db.instance->GetFileSystem().RegisterSubSystem(std::move(failing));
	CSVReaderOptions options;
	options.buffer_size_option.Set(262144);
	auto manager = CSVBufferManager::Open(*con.context, options, OpenFileInfo(path), false, nullptr);
	REQUIRE(manager->HasKnownBufferRanges());
	fail = true;
	REQUIRE_THROWS(manager->GetBuffer(1));
	REQUIRE_FALSE(fail);
	auto buffer = manager->GetBuffer(1);
	REQUIRE(buffer);
	REQUIRE(string(buffer->Ptr(), buffer->actual_size) == content.substr(262144, buffer->actual_size));
}

TEST_CASE("A range reader never writes into the windows that rows it handed out still hold", "[csv]") {
	string data;
	for (idx_t row = 0; row < 3000; row++) {
		data += to_string(row) + "," + string(200, static_cast<char>('a' + row % 26)) + "\n";
	}
	RowLexer lexer(RowDialect::PostgresCsv(',', '"', '"'), 2, 1 << 20, true);
	RowRangesTestStream source(data);
	RowRanges ranges(Allocator::DefaultAllocator(), lexer, source, 0, data.size(), (data.size() + 1) / 2, true);
	REQUIRE(ranges.RangeCount() == 2);
	idx_t range = DConstants::INVALID_INDEX;
	REQUIRE(ranges.Claim(range));
	REQUIRE(range == 0);
	RowRangeReader reader(ranges);
	reader.Start(0, true, false, false);
	REQUIRE(reader.Advance(1) == RowReadResult::ROWS);
	const auto first = reader.window.bytes;
	const string first_bytes(first->data, first->size);
	idx_t rows = reader.Ready();
	reader.Emit(reader.Ready());
	while (reader.Advance(STANDARD_VECTOR_SIZE) == RowReadResult::ROWS) {
		rows += reader.Ready();
		reader.Emit(reader.Ready());
	}
	ranges.Finish(0);
	const auto last = reader.window.bytes;
	REQUIRE(last != first);
	const string last_bytes(last->data, last->size);
	REQUIRE(ranges.Claim(range));
	REQUIRE(range == 1);
	reader.Start(1, true, false, false);
	REQUIRE(reader.window.bytes != first);
	REQUIRE(reader.window.bytes != last);
	while (reader.Advance(STANDARD_VECTOR_SIZE) == RowReadResult::ROWS) {
		rows += reader.Ready();
		reader.Emit(reader.Ready());
	}
	REQUIRE(rows == 3000);
	REQUIRE(string(first->data, first_bytes.size()) == first_bytes);
	REQUIRE(string(last->data, last_bytes.size()) == last_bytes);
}

TEST_CASE("Row ranges read nothing at the end of the input into a buffer that still has memory", "[csv]") {
	const string data = "a,b\nc,d\n";
	RowLexer lexer(RowDialect::PostgresCsv(',', '"', '"'), 2, 1 << 20, true);
	RowRangesTestStream source(data);
	RowRanges ranges(Allocator::DefaultAllocator(), lexer, source, 0, data.size(), DConstants::INVALID_INDEX, true);
	RowBytes bytes;
	ranges.ReadInto(bytes, data.size(), 16);
	REQUIRE(bytes.size == 0);
	REQUIRE(bytes.offset == data.size());
	REQUIRE(bytes.capacity == 1);
	REQUIRE(bytes.data != nullptr);
}

TEST_CASE("Intake ranges are not all finished while a reader cuts the next one", "[csv]") {
	const string data = "{\"a\":1}\n{\"a\":2}\n";
	PlainRowStream source(data);
	RowRanges ranges(Allocator::DefaultAllocator(), source, 0, NumericLimits<idx_t>::Maximum(), 1 << 20);
	REQUIRE(ranges.Intake());
	idx_t first = DConstants::INVALID_INDEX;
	idx_t second = DConstants::INVALID_INDEX;
	REQUIRE(ranges.Claim(first));
	REQUIRE(ranges.Claim(second));
	REQUIRE(first == 0);
	REQUIRE(second == 1);
	idx_t from = DConstants::INVALID_INDEX;
	REQUIRE(ranges.ClaimRead(first, from));
	REQUIRE(from == 0);
	ranges.FoundEnd(data.size());
	ranges.PassRead(data.size());
	RowClaim claim;
	REQUIRE(ranges.ClaimCut(first, claim));
	REQUIRE(claim.begin == 0);
	REQUIRE(!ranges.AllFinished());
	ranges.Cut(data.size(), nullptr, data.size(), CSVState::RECORD_SEPARATOR, DConstants::INVALID_INDEX);
	REQUIRE(!ranges.AllFinished());
	idx_t third = DConstants::INVALID_INDEX;
	REQUIRE(!ranges.Claim(third));
	REQUIRE(ranges.ClaimRead(second, from));
	REQUIRE(from == data.size());
	ranges.PassRead(from);
	REQUIRE(!ranges.ClaimCut(second, claim));
	REQUIRE(!ranges.AllFinished());
	REQUIRE(ranges.Finish(first));
	REQUIRE(ranges.AllFinished());
}

TEST_CASE("A stream of short rows is cut into pieces of about the range size", "[csv]") {
	struct Case {
		RowDialect dialect;
		string row;
		idx_t range_bytes;
	};
	const vector<Case> cases = {{RowDialect::PostgresText('\t'), "1\n", 64},
	                            {RowDialect::PostgresText('\t'), "\n", 64},
	                            {RowDialect::PostgresText('\t'), "1\r\n", 64},
	                            {RowDialect::DuckDB(TestGrammar(',', false, NewLineIdentifier::CARRY_ON)), "1\r\n", 65},
	                            {RowDialect::DuckDB(TestGrammar(',', false, NewLineIdentifier::CARRY_ON)), "1\r", 65},
	                            {RowDialect::DuckDB(TestGrammar(',', true, NewLineIdentifier::NOT_SET)), "1\n", 64}};
	for (const auto &test : cases) {
		string input;
		for (idx_t i = 0; i < 5000; i++) {
			input += test.row;
		}
		RowLexer lexer(test.dialect, 1, NumericLimits<idx_t>::Maximum(), true, false, true);
		RowRangesTestStream source(input);
		RowRanges ranges(Allocator::DefaultAllocator(), lexer, source, 0, NumericLimits<idx_t>::Maximum(),
		                 test.range_bytes, true);
		RowRangeReader reader(ranges);
		idx_t range = 0;
		idx_t rows = 0;
		idx_t longest = 0;
		while (ranges.Claim(range)) {
			if (!reader.StartIntake(range, true, true, false)) {
				continue;
			}
			longest = MaxValue(longest, reader.Stop() - reader.Begin());
			while (reader.Advance(STANDARD_VECTOR_SIZE) == RowReadResult::ROWS) {
				rows += reader.Ready();
				reader.Emit(reader.Ready());
			}
			ranges.Finish(range);
		}
		INFO("row " << test.row.size() << " bytes");
		REQUIRE(rows == 5000);
		REQUIRE(longest <= test.range_bytes + 2 * test.row.size());
	}
}

TEST_CASE("A text stream with runs of empty lines is cut inside them at the rows the whole input has", "[csv]") {
	std::mt19937_64 rng(7);
	for (const bool crlf : {false, true}) {
		string input;
		for (idx_t row = 0; row < 5000; row++) {
			input += rng() % 4 ? "" : "x\\\\y";
			input += crlf ? "\r\n" : "\n";
		}
		input += string(3000, '\n');
		RowLexer lexer(RowDialect::PostgresText('\t'), 1, NumericLimits<idx_t>::Maximum(), true, false, true);
		RowRangesTestStream whole(input);
		const auto expected = ReadRows(lexer, whole);
		REQUIRE(expected.size() == 8000);
		for (const idx_t range_bytes : {idx_t(1), idx_t(7), idx_t(64), idx_t(1) << 12}) {
			INFO("crlf " << crlf << ", range " << range_bytes);
			RowRangesTestStream stream(input);
			REQUIRE(ReadRanges(lexer, stream, range_bytes) == expected);
		}
	}
}

TEST_CASE("A stream whose unquoted escapes start mid-way is cut at the rows the whole input has", "[csv]") {
	auto grammar = TestGrammar(',', false, NewLineIdentifier::SINGLE_N);
	grammar.escape = '\\';
	grammar.quoted_escape = true;
	const auto dialect = RowDialect::DuckDB(grammar);
	REQUIRE(dialect.unquoted_escape);
	string input;
	for (idx_t row = 0; row < 400; row++) {
		input += to_string(row) + (row < 200 ? ",plain\n" : ",x\\,y\n");
	}
	RowLexer lexer(dialect, 2, NumericLimits<idx_t>::Maximum(), true, false, true);
	RowRangesTestStream whole(input);
	const auto expected = ReadRows(lexer, whole);
	REQUIRE(expected.size() == 400);
	for (const idx_t range_bytes : {idx_t(64), idx_t(1) << 10}) {
		RowRangesTestStream stream(input);
		REQUIRE(ReadRanges(lexer, stream, range_bytes) == expected);
	}
}

TEST_CASE("Rows longer than a piece are carried whole to the pieces after them", "[csv]") {
	struct Case {
		RowDialect dialect;
		string long_value;
	};
	string escaped;
	string quoted = "\"";
	string unquoted;
	for (idx_t i = 0; i < 20000; i++) {
		escaped += i % 7 ? "abcdefghi" : "abcdefg\\\n";
		quoted += i % 5 ? "abcdefghi" : i % 2 ? "abcdefg\"\"" : "abcdefgh\n";
		unquoted += "abcdefghi";
	}
	quoted += "\"";
	const vector<Case> cases = {{RowDialect::PostgresText('\t'), escaped},
	                            {RowDialect::PostgresCsv(',', '"', '"'), quoted},
	                            {RowDialect::DuckDB(TestGrammar(',', true, NewLineIdentifier::SINGLE_N)), unquoted},
	                            {RowDialect::DuckDB(TestGrammar(',', true, NewLineIdentifier::SINGLE_N)), quoted}};
	for (const auto &test : cases) {
		const char delimiter = test.dialect.backslash ? '\t' : ',';
		string input;
		for (idx_t row = 0; row < 300; row++) {
			input += to_string(row) + delimiter + (row % 100 == 50 ? test.long_value : "short") + "\n";
		}
		input += "end" + string(1, delimiter) + test.long_value;
		RowLexer lexer(test.dialect, 2, NumericLimits<idx_t>::Maximum(), true, false, true);
		RowRangesTestStream whole(input);
		const auto expected = ReadRows(lexer, whole);
		REQUIRE(expected.size() == 301);
		for (const idx_t range_bytes : {idx_t(1) << 10, idx_t(1) << 17}) {
			INFO("dialect backslash " << test.dialect.backslash << ", range " << range_bytes);
			RowRangesTestStream complete(input);
			REQUIRE(ReadRanges(lexer, complete, range_bytes) == expected);
			LiveRowStream live;
			vector<LexedRow> rows;
			std::thread reader_thread([&]() { rows = ReadRanges(lexer, live, range_bytes); });
			for (idx_t at = 0; at < input.size();) {
				const idx_t piece = MinValue<idx_t>(range_bytes / 3 + at % 97, input.size() - at);
				live.AwaitReader();
				live.Append(std::string_view(input).substr(at, piece));
				at += piece;
			}
			live.AwaitReader();
			live.Close();
			reader_thread.join();
			REQUIRE(rows == expected);
		}
	}
}

TEST_CASE("A failed intake releases the readers waiting for their turn", "[csv]") {
	const string data = "{\"a\":1}\n{\"a\":2}\n";
	PlainRowStream source(data);
	RowRanges ranges(Allocator::DefaultAllocator(), source, 0, NumericLimits<idx_t>::Maximum(), 1 << 20);
	idx_t pieces[4];
	for (auto &piece : pieces) {
		REQUIRE(ranges.Claim(piece));
	}
	idx_t from = DConstants::INVALID_INDEX;
	REQUIRE(ranges.ClaimRead(pieces[0], from));
	ranges.PassRead(from);
	bool cut = true;
	std::thread cutter([&]() {
		idx_t at;
		RowClaim claim;
		cut = ranges.ClaimRead(pieces[1], at);
		if (cut) {
			ranges.PassRead(at);
			cut = ranges.ClaimCut(pieces[1], claim);
		}
	});
	bool read = true;
	std::thread reader([&]() {
		idx_t at;
		read = ranges.ClaimRead(pieces[3], at);
	});
	ranges.FailIntake();
	cutter.join();
	reader.join();
	REQUIRE(!cut);
	REQUIRE(!read);
	RowClaim claim;
	REQUIRE(!ranges.ClaimCut(pieces[0], claim));
	idx_t next = DConstants::INVALID_INDEX;
	REQUIRE(!ranges.Claim(next));
}

TEST_CASE("Ranges of a file in memory ask for the bytes ahead of their reader and give back each range they finish",
          "[csv]") {
	const string row = "12345,abcdefgh\n";
	constexpr idx_t ROWS = 700000;
	string data;
	data.reserve(row.size() * ROWS);
	for (idx_t i = 0; i < ROWS; i++) {
		data += row;
	}
	MemoryRowSource source(data);
	RowLexer lexer(RowDialect::PostgresCsv(',', '"', '"'), 2, 1 << 20, true);
	const idx_t range_bytes = 3 * 1024 * 1024 + 7;
	RowRanges ranges(Allocator::DefaultAllocator(), lexer, source, 0, data.size(), range_bytes, true);
	REQUIRE(ranges.WholeFile());
	RowRangeReader reader(ranges);
	idx_t rows = 0;
	idx_t range = 0;
	while (ranges.Claim(range)) {
		reader.Start(range, true, false, false);
		while (true) {
			const auto result = reader.Advance(STANDARD_VECTOR_SIZE);
			if (result == RowReadResult::DONE) {
				break;
			}
			REQUIRE(result == RowReadResult::ROWS);
			rows += reader.Ready();
			reader.Emit(reader.Ready());
		}
		ranges.Finish(range);
	}
	REQUIRE(rows == ROWS);

	REQUIRE(source.dones.size() == ranges.RangeCount());
	for (idx_t i = 0; i < ranges.RangeCount(); i++) {
		REQUIRE(source.dones[i] == make_pair(ranges.RangeBegin(i), ranges.RangeEnd(i)));
	}
	REQUIRE(source.wants.size() <= data.size() / RowWindow::PREFETCH_STEP + 2 * ranges.RangeCount());
	auto wants = source.wants;
	std::sort(wants.begin(), wants.end());
	idx_t covered = 0;
	for (auto &want : wants) {
		REQUIRE(want.first < want.second);
		REQUIRE(want.second <= data.size());
		REQUIRE(want.second - want.first <= RowWindow::PREFETCH_BYTES + RowWindow::PIECE);
		REQUIRE(want.first <= covered);
		covered = MaxValue(covered, want.second);
	}
	REQUIRE(covered == data.size());
	auto reads = source.reads;
	std::sort(reads.begin(), reads.end());
	covered = 0;
	for (auto &read : reads) {
		REQUIRE(read.first < read.second);
		REQUIRE(read.second <= data.size());
		REQUIRE(read.first <= covered);
		covered = MaxValue(covered, read.second);
	}
	REQUIRE(covered == data.size());

	source.wants.clear();
	source.reads.clear();
	RowBytes bytes;
	ranges.ReadInto(bytes, 100, 50);
	REQUIRE(bytes.data == source.Memory().data + 100);
	REQUIRE(bytes.size == 50);
	ranges.ReadInto(bytes, data.size(), 50);
	REQUIRE(bytes.size == 0);
	REQUIRE(source.wants == vector<pair<idx_t, idx_t>> {{100, 150}});
	REQUIRE(source.reads == vector<pair<idx_t, idx_t>> {{100, 150}});
}

TEST_CASE("A row source over compressed pieces reads any span of what they decompress to", "[csv]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto compressed =
	    FileSystem::GetFileSystem(*con.context).GetCompressedFileSystem(FileCompressionType::ZSTD, "pieces.zst");
	REQUIRE(compressed);
	auto compressor = compressed->CreatePieces()->CreateCompressor();
	string expected;
	string file;
	for (idx_t piece = 0; piece < 5; piece++) {
		string input;
		if (piece != 2) {
			for (idx_t row = 0; row < 3000 * (piece + 1); row++) {
				input += to_string(piece) + "," + to_string(row * 7919 % 100003) + "\n";
			}
		}
		CompressedPiece bytes;
		compressor->Compress(const_data_ptr_cast(input.data()), input.size(), bytes);
		file.append(const_char_ptr_cast(bytes.data.get()), bytes.size);
		expected += input;
		if (piece == 1) {
			const char skippable[] = {'\x5a', '\x2a', '\x4d', '\x18', '\x03', '\x00', '\x00', '\x00', 'a', 'b', 'c'};
			file.append(skippable, sizeof(skippable));
		}
	}
	vector<PieceSpan> pieces;
	REQUIRE(compressed->FindPieces(const_data_ptr_cast(file.data()), file.size(), pieces));
	REQUIRE(pieces.size() == 5);
	REQUIRE(pieces[2].output_size == 0);
	REQUIRE(!compressed->FindPieces(const_data_ptr_cast(file.data()), file.size() - 1, pieces));
	const string unknown_size("\x28\xb5\x2f\xfd\x00\x00\x19\x00\x00"
	                          "abc",
	                          12);
	const string huge_size("\x28\xb5\x2f\xfd\xe0\x00\x00\x00\x08\x00\x00\x00\x00", 13);
	const string short_skippable("\x50\x2a\x4d\x18\x64\x00\x00\x00"
	                             "abc",
	                             11);
	for (const auto &tail : {unknown_size, huge_size, short_skippable, string("xyz")}) {
		const string bad = file + tail;
		REQUIRE(!compressed->FindPieces(const_data_ptr_cast(bad.data()), bad.size(), pieces));
	}

	const string short_frame("\x28\xb5\x2f\xfd\x20\x06\x19\x00\x00"
	                         "abc",
	                         12);
	string lying = file + short_frame;
	REQUIRE(compressed->FindPieces(const_data_ptr_cast(lying.data()), lying.size(), pieces));
	REQUIRE(pieces.back().output_size == 6);
	{
		auto lying_mapping = make_shared_ptr<RecordingMapping>(data_ptr_cast(&lying[0]), lying.size());
		PiecedRowSource lying_source(lying_mapping, *compressed, pieces);
		string out(6, '\0');
		REQUIRE_THROWS_AS(lying_source.Read(&out[0], 6, expected.size()), IOException);
		REQUIRE_THROWS_AS(lying_source.Read(&out[0], 5, expected.size() + 1), IOException);
	}

	auto mapping = make_shared_ptr<RecordingMapping>(data_ptr_cast(&file[0]), file.size());
	REQUIRE(compressed->FindPieces(const_data_ptr_cast(file.data()), file.size(), pieces));
	PiecedRowSource source(mapping, *compressed, pieces);
	REQUIRE(source.Size() == expected.size());
	REQUIRE(source.End().GetIndex() == expected.size());

	std::mt19937 random(42);
	for (idx_t read = 0; read < 300; read++) {
		const idx_t offset = random() % (expected.size() + 1);
		const idx_t count = random() % 40000;
		string out(count, '\0');
		const idx_t got = source.Read(&out[0], count, offset);
		CAPTURE(offset, count);
		REQUIRE(got == MinValue(count, expected.size() - offset));
		REQUIRE(out.substr(0, got) == expected.substr(offset, got));
	}
	string whole(expected.size(), '\0');
	REQUIRE(source.Read(&whole[0], whole.size(), 0) == expected.size());
	REQUIRE(whole == expected);
	REQUIRE(!mapping->prefetched.empty());

	auto cursor = source.Cursor();
	REQUIRE(cursor);
	for (idx_t start : {idx_t(0), idx_t(12345), idx_t(100)}) {
		string streamed;
		char step[1000];
		for (idx_t offset = start;;) {
			const idx_t got = cursor->Read(step, sizeof(step), offset);
			if (got == 0) {
				break;
			}
			streamed.append(step, got);
			offset += got;
		}
		CAPTURE(start);
		REQUIRE(streamed == expected.substr(start));
	}
}

TEST_CASE("A mapped row source prefetches the bytes the page cache lacks before they are read", "[csv]") {
	string data(10000, '\0');
	for (idx_t i = 0; i < data.size(); i++) {
		data[i] = static_cast<char>('a' + i % 26);
	}
	auto mapping = make_shared_ptr<RecordingMapping>(data_ptr_cast(&data[0]), data.size());
	MappedRowSource source(mapping);
	REQUIRE(source.Memory().data == data.data());
	REQUIRE(source.End().GetIndex() == data.size());

	source.Want(100, 300);
	REQUIRE(mapping->prefetched == vector<pair<idx_t, idx_t>> {{100, 200}});
	char out[64];
	REQUIRE(source.Read(out, sizeof(out), data.size() - 10) == 10);
	REQUIRE(memcmp(out, data.data() + data.size() - 10, 10) == 0);
	REQUIRE(mapping->prefetched.back() == make_pair(idx_t(data.size() - 10), idx_t(10)));
	REQUIRE(source.Read(out, sizeof(out), data.size()) == 0);
	REQUIRE(mapping->prefetched.size() == 2);

	mapping->cached = true;
	source.Want(0, 5000);
	REQUIRE(source.Read(out, 8, 0) == 8);
	REQUIRE(memcmp(out, data.data(), 8) == 0);
	REQUIRE(mapping->prefetched.size() == 2);

	mapping->cached = false;
	source.Want(0, 4000);
	REQUIRE(mapping->prefetched.size() == 2);
	source.Want(4000, 6000);
	REQUIRE(mapping->prefetched.back() == make_pair(idx_t(5000), idx_t(1000)));
	source.ReadInPlace(0, 6000);

	source.Done(0, 4096);
	REQUIRE(mapping->released == vector<pair<idx_t, idx_t>> {{0, 4096}});

	TestMapping plain(data_ptr_cast(&data[0]), data.size());
	REQUIRE(!plain.IsCached(0, data.size()));
	plain.Release(0, data.size());
	REQUIRE(memcmp(plain.GetData(0, data.size()), data.data(), data.size()) == 0);
}

TEST_CASE("A small file, a file gone from its path, or one that grew after it was opened, is read instead of mapped",
          "[csv]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;
	auto &fs = FileSystem::GetFileSystem(context);
	CSVReaderOptions options;
	auto write = [](const string &path, idx_t rows) {
		std::ofstream out(path, std::ios::binary);
		out << "a,b\n";
		for (idx_t row = 0; row < rows; row++) {
			out << "1,2\n";
		}
	};
	const idx_t rows = MAPPED_FILE_MIN_BYTES / 4;

	const auto small = TestCreatePath("small_unmapped.csv");
	write(small, rows - 2);
	auto small_handle = CSVFileHandle::OpenFile(context, OpenFileInfo(small), options, CachingMode::NO_CACHING);
	REQUIRE(small_handle->FileSize() < MAPPED_FILE_MIN_BYTES);
	REQUIRE(!MappedRowSource::Open(context, *small_handle, small));

	const auto gone = TestCreatePath("gone_after_open.csv");
	write(gone, rows);
	auto gone_handle = CSVFileHandle::OpenFile(context, OpenFileInfo(gone), options, CachingMode::NO_CACHING);
	fs.RemoveFile(gone);
	REQUIRE(!MappedRowSource::Open(context, *gone_handle, gone));

	const auto path = TestCreatePath("grown_after_open.csv");
	write(path, rows);
	auto handle = CSVFileHandle::OpenFile(context, OpenFileInfo(path), options, CachingMode::NO_CACHING);
	{
		std::ofstream out(path, std::ios::binary | std::ios::app);
		out << "3,4\n";
	}
	REQUIRE(!MappedRowSource::Open(context, *handle, path));
	auto reopened = CSVFileHandle::OpenFile(context, OpenFileInfo(path), options, CachingMode::NO_CACHING);
	auto mapped = MappedRowSource::Open(context, *reopened, path);
	REQUIRE(mapped);
	REQUIRE(mapped->End().GetIndex() == 4 + rows * 4 + 4);
}

TEST_CASE("A whole file is mapped from its minimum size up and read below it, with the file's bytes either way, "
          "in the page cache or not, kept by a holder after its handle closes",
          "[csv]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;
	auto &fs = FileSystem::GetFileSystem(context);
	for (const idx_t size :
	     {idx_t(0), idx_t(100), MAPPED_FILE_MIN_BYTES - 1, MAPPED_FILE_MIN_BYTES, MAPPED_FILE_MIN_BYTES + 4097}) {
		const auto path = TestCreatePath("whole_file_" + to_string(size));
		string bytes(size, '\0');
		for (idx_t i = 0; i < size; i++) {
			bytes[i] = static_cast<char>('a' + i % 26);
		}
		{
			std::ofstream out(path, std::ios::binary);
			out << bytes;
		}
#if defined(__linux__)
		{
			const int fd = open(path.c_str(), O_RDONLY);
			REQUIRE(fd >= 0);
			REQUIRE(fdatasync(fd) == 0);
			REQUIRE(posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) == 0);
			close(fd);
		}
#endif
		auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
		auto file = WholeFile::Read(context, *handle);
		const auto data = file->data;
		REQUIRE(file->size == size);
		RowBufferHolder holder(std::move(file));
		handle.reset();
		REQUIRE(string(data, size) == bytes);
		if (size == 0) {
			continue;
		}
		{
			std::fstream out(path, std::ios::binary | std::ios::in | std::ios::out);
			out.put('#');
		}
		REQUIRE((data[0] == '#') == (size >= MAPPED_FILE_MIN_BYTES));
	}
}

TEST_CASE("Range lines count the lines before a range once every range before it finished", "[formats]") {
	RangeLines lines;
	REQUIRE(lines.Known(0));
	REQUIRE(lines.Before(0) == 0);
	REQUIRE_FALSE(lines.Add(2, 5));
	REQUIRE_FALSE(lines.Known(1));
	REQUIRE(lines.Before(3) == 5);
	REQUIRE(lines.Add(0, 3));
	REQUIRE(lines.Known(1));
	REQUIRE_FALSE(lines.Known(2));
	REQUIRE(lines.Before(1) == 3);
	REQUIRE(lines.Add(1, 4));
	REQUIRE(lines.Known(3));
	REQUIRE_FALSE(lines.Known(4));
	REQUIRE(lines.Before(3) == 12);
	REQUIRE(lines.Before(2) == 7);
	REQUIRE_FALSE(lines.Add(1, 1));
	REQUIRE(lines.Before(3) == 13);
	REQUIRE(lines.Before(2) == 8);
}

TEST_CASE("A range failure keeps the earliest failing record and names its line once the ranges before are known",
          "[formats]") {
	RangeLines lines;
	RangeFailure failure;
	REQUIRE_FALSE(failure.Failed());
	REQUIRE_FALSE(failure.Line(lines).IsValid());
	REQUIRE(failure.Fail(3, 7));
	REQUIRE(failure.Failed());
	REQUIRE_FALSE(failure.Fail(3, 7));
	REQUIRE_FALSE(failure.Fail(3, 9));
	REQUIRE_FALSE(failure.Fail(4, 0));
	REQUIRE(failure.Fail(3, 2));
	REQUIRE(failure.Fail(2, 5));
	REQUIRE_FALSE(failure.Line(lines).IsValid());
	lines.Add(0, 10);
	REQUIRE_FALSE(failure.Line(lines).IsValid());
	lines.Add(1, 20);
	REQUIRE(failure.Line(lines).GetIndex() == 36);
	REQUIRE(failure.Fail(0, 4));
	REQUIRE(failure.Line(lines).GetIndex() == 5);
}
