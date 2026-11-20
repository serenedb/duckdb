#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/fault_points.hpp"
#include "duckdb/formats/row_lexer.hpp"
#include "duckdb/formats/row_ranges.hpp"
#include "duckdb/formats/row_stream.hpp"
#include "duckdb/main/client_context.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
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

} // namespace

#ifdef SDB_FAULT_INJECTION
TEST_CASE("A stream range is empty when another reader finds the stream ends before it", "[csv]") {
	string data;
	for (idx_t row = 0; row < 50; row++) {
		data += "a,b\n";
	}
	const auto dialect = RowDialect::PostgresCsv(',', '"', '"');
	RowLexer lexer(dialect, 2, 1 << 20, true);
	RowRangesTestStream source(data);
	RowRanges ranges(lexer, source, 0, NumericLimits<idx_t>::Maximum(), 64, true);
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
	RowRanges unknown_end(lexer, plain, 0, NumericLimits<idx_t>::Maximum(), DConstants::INVALID_INDEX, true);
	RowBytes bytes;
	unknown_end.ReadInto(bytes, 0, 100);
	REQUIRE(bytes.size == data.size());
	REQUIRE(unknown_end.Size() == data.size());
	unknown_end.ReadInto(bytes, data.size(), 10);
	REQUIRE(bytes.size == 0);

	RowRanges beyond(lexer, plain, 0, NumericLimits<idx_t>::Maximum(), DConstants::INVALID_INDEX, true);
	RowBytes empty;
	beyond.ReadInto(empty, 100, 10);
	REQUIRE(empty.size == 0);
	REQUIRE(beyond.Size() == NumericLimits<idx_t>::Maximum());

	RowRangesTestStream known(data);
	RowRanges known_end(lexer, known, 0, NumericLimits<idx_t>::Maximum(), DConstants::INVALID_INDEX, true);
	RowBytes known_bytes;
	known_end.ReadInto(known_bytes, 4, 100);
	REQUIRE(known_bytes.size == 4);
	REQUIRE(known_end.Size() == data.size());
}

TEST_CASE("A range reader reads and counts the rows of a stream", "[csv]") {
	const string data = "a,b\nc,d\n";
	RowLexer lexer(RowDialect::PostgresCsv(',', '"', '"'), 2, 1 << 20, true);
	PlainRowStream source(data);

	RowRanges ranges(lexer, source, 0, NumericLimits<idx_t>::Maximum(), DConstants::INVALID_INDEX, true);
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

	RowRanges counted(lexer, source, 0, NumericLimits<idx_t>::Maximum(), DConstants::INVALID_INDEX, true);
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
	RowRanges ranges(lexer, source, 0, data.size(), DConstants::INVALID_INDEX, true);
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
	RowRanges crlf_ranges(crlf_lexer, crlf_source, 0, crlf.size(), DConstants::INVALID_INDEX, true);
	RowBytes crlf_bytes;
	crlf_ranges.ReadInto(crlf_bytes, 0, crlf.size());
	const auto after_cr = crlf_ranges.LexTransfer(crlf_bytes, 0, crlf.size(), CSVState::CARRIAGE_RETURN);
	REQUIRE(after_cr.computed);
	REQUIRE(after_cr.end.states[1] == CSVState::RECORD_SEPARATOR);

	const string unterminated = "abc";
	RowRangesTestStream unterminated_source(unterminated);
	RowRanges unterminated_ranges(lexer, unterminated_source, 0, unterminated.size(), DConstants::INVALID_INDEX, true);
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
