#include "absl/synchronization/mutex.h"
#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/fault_points.hpp"
#include "duckdb/formats/row_lexer.hpp"
#include "duckdb/formats/row_ranges.hpp"
#include "duckdb/formats/row_stream.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include <thread>

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
		reader.Start(range, true, true, false);
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
	idx_t range = 0;
	REQUIRE(ranges.Claim(range));
	RowRangeReader reader(ranges);
	reader.Start(range, true, false, false);
	vector<LexedRow> rows;
	source.Append("1\t2\n3\t4\n5\t");
	REQUIRE(reader.Advance(STANDARD_VECTOR_SIZE) == RowReadResult::ROWS);
	REQUIRE(reader.Ready() == 2);
	TakeRows(reader, rows);
	source.Append("6\n7\t8");
	REQUIRE(reader.Advance(STANDARD_VECTOR_SIZE) == RowReadResult::ROWS);
	REQUIRE(reader.Ready() == 1);
	TakeRows(reader, rows);
	source.Close();
	REQUIRE(reader.Advance(STANDARD_VECTOR_SIZE) == RowReadResult::ROWS);
	REQUIRE(reader.Ready() == 1);
	TakeRows(reader, rows);
	REQUIRE(reader.Advance(STANDARD_VECTOR_SIZE) == RowReadResult::DONE);
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
					reader.Start(range, true, true, false);
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
	                                     RowDialect::DuckDB(TestGrammar(',', false, NewLineIdentifier::CARRY_ON))};
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
		std::thread reader_thread([&]() { rows = ReadRows(lexer, live); });
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
		REQUIRE(rows == expected);
	}
}

#ifdef SDB_FAULT_INJECTION
TEST_CASE("A stream range is empty when another reader finds the stream ends before it", "[csv]") {
	string data;
	for (idx_t row = 0; row < 50; row++) {
		data += "a,b\n";
	}
	const auto dialect = RowDialect::PostgresCsv(',', '"', '"');
	RowLexer lexer(dialect, 2, 1 << 20, true);
	RowRangesTestStream source(data);
	RowRanges ranges(Allocator::DefaultAllocator(), lexer, source, 0, NumericLimits<idx_t>::Maximum(), 64, true);
	idx_t range = 0;
	for (idx_t claimed = 0; claimed <= 10; claimed++) {
		REQUIRE(ranges.Claim(range));
	}
	REQUIRE(range == 10);

	RowRangeReader reader(ranges);
	reader.Start(range, true, false, false);
	FaultPoints::Arm("row_range_lex");
	auto result = RowReadResult::ROWS;
	std::thread worker([&]() { result = reader.Advance(STANDARD_VECTOR_SIZE); });
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!FaultPoints::Armed("row_range_lex:parked") && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const bool parked = FaultPoints::Armed("row_range_lex:parked");
	ranges.FoundEnd(data.size());
	FaultPoints::Arm("row_range_lex:release");
	worker.join();
	FaultPoints::Clear();
	REQUIRE(parked);
	REQUIRE(result == RowReadResult::DONE);
}
#endif

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
	REQUIRE(fields.events.size() == 1);
	REQUIRE(fields.events[0].row == 1);
	REQUIRE(fields.anomalies.size() == 1);
	REQUIRE(fields.anomalies[0].row == 1);
	REQUIRE(fields.extras.size() == 1);
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
