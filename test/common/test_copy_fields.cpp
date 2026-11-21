#include "catch.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/formats/copy_encoding.hpp"
#include "duckdb/formats/copy_fields.hpp"
#include "duckdb/formats/copy_options.hpp"
#include "duckdb/formats/row_ranges.hpp"

#include <random>

using namespace duckdb;

namespace {

class SpanSource : public RowSource {
public:
	explicit SpanSource(std::string_view bytes_p) : bytes(bytes_p) {
	}

	idx_t Read(char *data, idx_t count, idx_t offset) const override {
		const idx_t available = offset < bytes.size() ? MinValue<idx_t>(count, bytes.size() - offset) : 0;
		memcpy(data, bytes.data() + offset, available);
		return available;
	}

private:
	std::string_view bytes;
};

struct Lexed {
	vector<vector<std::string_view>> rows;
	bool unfinished = false;
};

Lexed Lex(const RowDialect &dialect, idx_t columns, const string &input, idx_t max_rows) {
	RowLexer lexer(dialect, columns, NumericLimits<idx_t>::Maximum(), true, false, true);
	SpanSource source(input);
	RowRanges ranges(Allocator::DefaultAllocator(), lexer, source, 0, input.size(), DConstants::INVALID_INDEX, true);
	ranges.SetWholeFile(nullptr, input.data());
	RowRangeReader reader(ranges);
	reader.Start(0, true, false, false);
	Lexed result;
	while (true) {
		const auto read = reader.Advance(max_rows);
		if (read == RowReadResult::DONE) {
			break;
		}
		const idx_t count = MinValue<idx_t>(reader.Ready(), max_rows);
		auto &fields = reader.fields;
		auto &window = *reader.window.bytes;
		const idx_t shift = reader.cursor.anchor - window.offset;
		for (idx_t row = 0; row < count; row++) {
			const auto *anomaly = fields.anomalies.empty() ? nullptr : fields.Anomaly(row);
			const idx_t field_count = anomaly ? anomaly->fields : columns;
			vector<std::string_view> row_fields;
			idx_t begin = fields.rows[row] + shift;
			for (idx_t column = 0; column < field_count; column++) {
				const uint32_t end_bits = column < columns ? fields.ends[row * columns + column]
				                                           : fields.extras[anomaly->extras + column - columns];
				const idx_t end = (end_bits & ROW_FIELD_MASK) + shift;
				row_fields.emplace_back(window.data + begin, end - begin);
				begin = end + 1;
			}
			result.rows.push_back(std::move(row_fields));
		}
		result.unfinished = reader.cursor.unfinished;
		reader.Emit(count);
	}
	return result;
}

struct PostgresCsvField {
	string value;
	bool quoted = false;
};

vector<PostgresCsvField> SplitCsvLine(std::string_view line, char delimiter, char quote, char escape,
                                      bool &unterminated) {
	vector<PostgresCsvField> fields;
	idx_t i = 0;
	while (true) {
		PostgresCsvField field;
		bool delimited = false;
		while (true) {
			bool quoted = false;
			while (i < line.size()) {
				const char c = line[i++];
				if (c == delimiter) {
					delimited = true;
					break;
				}
				if (c == quote) {
					field.quoted = quoted = true;
					break;
				}
				field.value.push_back(c);
			}
			if (!quoted) {
				break;
			}
			while (true) {
				if (i >= line.size()) {
					unterminated = true;
					break;
				}
				const char c = line[i++];
				if (c == escape && i < line.size() && (line[i] == escape || line[i] == quote)) {
					field.value.push_back(line[i++]);
					continue;
				}
				if (c == quote) {
					break;
				}
				field.value.push_back(c);
			}
			if (unterminated) {
				break;
			}
		}
		fields.push_back(std::move(field));
		if (!delimited || unterminated) {
			return fields;
		}
	}
}

struct PostgresCsvReference {
	vector<vector<PostgresCsvField>> rows;
	bool unterminated = false;
};

PostgresCsvReference ParseCsv(std::string_view input, char delimiter, char quote, char escape) {
	PostgresCsvReference result;
	const char quoted_escape = escape == quote ? '\0' : escape;
	idx_t start = 0;
	bool in_quote = false;
	bool last_was_escape = false;
	const auto line = [&](idx_t end, bool at_eof) {
		auto text = input.substr(start, end - start);
		if (!text.empty() && text.back() == '\r' && !(at_eof && in_quote)) {
			text.remove_suffix(1);
		}
		bool unterminated = false;
		result.rows.push_back(SplitCsvLine(text, delimiter, quote, escape, unterminated));
		result.unterminated |= unterminated;
	};
	for (idx_t i = 0; i < input.size(); i++) {
		const char c = input[i];
		if (in_quote && c == quoted_escape) {
			last_was_escape = !last_was_escape;
		}
		if (c == quote && !last_was_escape) {
			in_quote = !in_quote;
		}
		if (c != quoted_escape) {
			last_was_escape = false;
		}
		if (!in_quote && c == '\n') {
			line(i, false);
			start = i + 1;
		}
	}
	if (start < input.size()) {
		line(input.size(), true);
	}
	result.unterminated |= in_quote;
	return result;
}

CopyFromRules CsvRules(char delimiter, char quote, char escape) {
	CopyFromRules rules;
	rules.options.format = CopyFormat::CSV;
	rules.options.delimiter = delimiter;
	rules.options.quote = quote;
	rules.options.escape = escape;
	rules.options.null_str = "\x01";
	return rules;
}

vector<std::string_view> TextLines(std::string_view input) {
	vector<std::string_view> lines;
	const auto line = [&](std::string_view text) {
		idx_t before_cr = 0;
		for (idx_t j = text.size(); j > 1 && text[j - 2] == '\\'; j--) {
			before_cr++;
		}
		if (!text.empty() && text.back() == '\r' && before_cr % 2 == 0) {
			text.remove_suffix(1);
		}
		lines.push_back(text);
	};
	idx_t start = 0;
	idx_t backslashes = 0;
	for (idx_t i = 0; i < input.size(); i++) {
		if (input[i] == '\\') {
			backslashes++;
			continue;
		}
		if (input[i] == '\n' && backslashes % 2 == 0) {
			line(input.substr(start, i - start));
			start = i + 1;
		}
		backslashes = 0;
	}
	if (start < input.size()) {
		line(input.substr(start));
	}
	return lines;
}

vector<std::string_view> SplitTextLine(std::string_view line, char delimiter) {
	vector<std::string_view> fields;
	idx_t start = 0;
	for (idx_t i = 0; i < line.size(); i++) {
		if (line[i] == '\\') {
			i++;
			continue;
		}
		if (line[i] == delimiter) {
			fields.push_back(line.substr(start, i - start));
			start = i + 1;
		}
	}
	fields.push_back(line.substr(start));
	return fields;
}

string Escaped(std::string_view input) {
	string result;
	for (const char c : input) {
		switch (c) {
		case '\n':
			result += "\\n";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\t':
			result += "\\t";
			break;
		case '\\':
			result += "\\\\";
			break;
		default:
			result.push_back(c);
		}
	}
	return result;
}

} // namespace

TEST_CASE("COPY fields of PostgreSQL's CSV, as lexed and read, match PostgreSQL's reading", "[copy]") {
	const char alphabet[] = {'a', ',', '"', '\n', '\r', '\\', 'b', ' '};
	std::mt19937_64 rng(17);
	for (int iter = 0; iter < 100000; iter++) {
		const char escape = iter % 4 == 3 ? '\\' : '"';
		const idx_t columns = 1 + rng() % 3;
		const idx_t length = rng() % 160;
		string input;
		for (idx_t i = 0; i < length; i++) {
			input.push_back(alphabet[rng() % sizeof(alphabet)]);
		}
		INFO("iteration " << iter << " columns " << columns << " input " << Escaped(input));
		const auto expected = ParseCsv(input, ',', '"', escape);
		const auto lexed = Lex(RowDialect::PostgresCsv(',', '"', escape), columns, input, 1 + rng() % 6);
		if (expected.unterminated) {
			REQUIRE(lexed.unfinished);
			continue;
		}
		REQUIRE(!lexed.unfinished);
		REQUIRE(lexed.rows.size() == expected.rows.size());
		const auto rules = CsvRules(',', '"', escape);
		string out(input.size() + 1, '\0');
		for (idx_t row = 0; row < lexed.rows.size(); row++) {
			auto &fields = lexed.rows[row];
			auto &reference = expected.rows[row];
			REQUIRE(fields.size() == reference.size());
			for (idx_t column = 0; column < MinValue(fields.size(), columns); column++) {
				std::string_view value;
				REQUIRE(ReadCopyField(rules, column, fields[column], &out[0], value) == CopyFieldKind::VALUE);
				REQUIRE((fields[column].find('"') != std::string_view::npos) == reference[column].quoted);
				REQUIRE(string(value) == reference[column].value);
			}
		}
	}
}

TEST_CASE("COPY fields of PostgreSQL's text format, as lexed, match its lines and delimiters", "[copy]") {
	const char alphabet[] = {'a', '\t', '\n', '\\', '\r', '.', 'N', 'b'};
	std::mt19937_64 rng(7);
	for (int iter = 0; iter < 100000; iter++) {
		const idx_t columns = 1 + rng() % 3;
		const idx_t length = rng() % 160;
		string input;
		for (idx_t i = 0; i < length; i++) {
			input.push_back(alphabet[rng() % sizeof(alphabet)]);
		}
		INFO("iteration " << iter << " columns " << columns << " input " << Escaped(input));
		const auto lines = TextLines(input);
		const auto lexed = Lex(RowDialect::PostgresText('\t'), columns, input, 1 + rng() % 8);
		REQUIRE(lexed.rows.size() == lines.size());
		for (idx_t row = 0; row < lines.size(); row++) {
			const auto expected = SplitTextLine(lines[row], '\t');
			const auto joined = [](const vector<std::string_view> &fields) {
				return StringUtil::Join(fields, "|", [](std::string_view field) { return Escaped(field); });
			};
			INFO("row " << row << " lexed " << joined(lexed.rows[row]) << " expected " << joined(expected));
			REQUIRE(lexed.rows[row] == expected);
		}
	}
}

TEST_CASE("COPY's text format unescapes like PostgreSQL", "[copy]") {
	string out(64, '\0');
	REQUIRE(UnescapePostgresText("a\\tb\\nc\\\\d", &out[0]) == "a\tb\nc\\d");
	REQUIRE(UnescapePostgresText("\\x41\\101\\x\\q\\", &out[0]) == "AAxq\\");
	REQUIRE(UnescapePostgresText("\\777", &out[0]) == "\xFF");
	REQUIRE_THROWS(UnescapePostgresText("a\\.b", &out[0]));
}

TEST_CASE("COPY takes the line break of its first line, however its input is split", "[copy]") {
	struct Case {
		CopyFormat format;
		string input;
		char expected;
	};
	const vector<Case> cases = {
	    {CopyFormat::TEXT, "a\tb\nc", '\n'},    {CopyFormat::TEXT, "a\tb\r\nc", '\n'},
	    {CopyFormat::TEXT, "a\tb\rc", '\r'},    {CopyFormat::TEXT, "a\\\nb\rc\n", '\r'},
	    {CopyFormat::TEXT, "a\\\rb\r\n", '\n'}, {CopyFormat::TEXT, "ab\r", '\r'},
	    {CopyFormat::TEXT, "ab\\", '\n'},       {CopyFormat::TEXT, "", '\n'},
	    {CopyFormat::CSV, "\"a\nb\"\rc", '\r'}, {CopyFormat::CSV, "\"a\"\"\r\"\r\n", '\n'},
	    {CopyFormat::CSV, "a,b", '\n'},
	};
	for (const auto &test : cases) {
		for (const char escape : {'"', '\\'}) {
			CopyFormatOptions options;
			options.format = test.format;
			options.escape = escape;
			for (idx_t split = 0; split <= test.input.size(); split++) {
				INFO("input " << Escaped(test.input) << " escape " << escape << " split " << split);
				CopyLineBreakFinder finder(options);
				char found = finder.Feed(std::string_view(test.input).substr(0, split));
				if (found == '\0') {
					found = finder.Feed(std::string_view(test.input).substr(split));
				}
				if (found == '\0') {
					found = finder.Finish();
				}
				REQUIRE(found == test.expected);
			}
		}
	}
	CopyFormatOptions options;
	options.format = CopyFormat::CSV;
	options.escape = '\\';
	const string escaped_quote = "\"a\\\"\nb\"\r";
	for (idx_t split = 0; split <= escaped_quote.size(); split++) {
		INFO("split " << split);
		CopyLineBreakFinder finder(options);
		char found = finder.Feed(std::string_view(escaped_quote).substr(0, split));
		if (found == '\0') {
			found = finder.Feed(std::string_view(escaped_quote).substr(split));
		}
		REQUIRE((found == '\0' ? finder.Finish() : found) == '\r');
	}
}

namespace {

string BeforeEndMarker(const string &input) {
	bool line_start = true;
	for (idx_t i = 0; i < input.size(); i++) {
		if (input[i] != '\\') {
			line_start = input[i] == '\n';
			continue;
		}
		if (line_start && i + 1 < input.size() && input[i + 1] == '.') {
			if (i + 2 == input.size() || input[i + 2] == '\n' ||
			    (input[i + 2] == '\r' && i + 3 < input.size() && input[i + 3] == '\n')) {
				return input.substr(0, i);
			}
		}
		line_start = false;
		i++;
	}
	return input;
}

string ScanForEndMarker(const string &input, const vector<idx_t> &cuts) {
	CopyEndMarkerScanner scanner;
	string result;
	idx_t from = 0;
	for (const idx_t cut : cuts) {
		const auto scanned = scanner.Scan(std::string_view(input).substr(from, cut - from));
		result += scanned.carry;
		result += scanned.data;
		from = cut;
	}
	const auto scanned = scanner.Scan(std::string_view(input).substr(from));
	result += scanned.carry;
	result += scanned.data;
	result += scanner.Finish();
	return result;
}

} // namespace

TEST_CASE("COPY's text stream ends at a line of only the end marker, however it is split", "[copy]") {
	const vector<string> cases = {"a\n\\.\nb",   "a\\\n\\.\nb", "\\.\r\nb",     "\\.",       "a\n\\.\r",
	                              "\\\\.\nb",    "a\\.\nb",     "x\\\\\n\\.\n", "\\\\\\.\n", "a\n\\.x\n\\.\n",
	                              "a\r\\.\nb\n", "\\",          "\\.\rb\n\\.\n"};
	const vector<string> expected = {"a\n",         "a\\\n\\.\nb", "",        "",          "a\n\\.\r",
	                                 "\\\\.\nb",    "a\\.\nb",     "x\\\\\n", "\\\\\\.\n", "a\n\\.x\n",
	                                 "a\r\\.\nb\n", "\\",          "\\.\rb\n"};
	for (idx_t c = 0; c < cases.size(); c++) {
		const auto &input = cases[c];
		REQUIRE(BeforeEndMarker(input) == expected[c]);
		for (idx_t split = 0; split <= input.size(); split++) {
			INFO("input " << Escaped(input) << " split " << split);
			REQUIRE(ScanForEndMarker(input, {split}) == expected[c]);
		}
	}
	std::mt19937_64 rng(23);
	const string alphabet = "\\\\\\.\n\r a";
	for (idx_t round = 0; round < 20000; round++) {
		string input(rng() % 300, ' ');
		for (auto &c : input) {
			c = alphabet[rng() % alphabet.size()];
		}
		vector<idx_t> cuts;
		for (idx_t at = rng() % 80; at < input.size(); at += 1 + rng() % 80) {
			cuts.push_back(at);
		}
		INFO("input " << Escaped(input));
		REQUIRE(ScanForEndMarker(input, cuts) == BeforeEndMarker(input));
	}
}

TEST_CASE("COPY reads null and default markers only where PostgreSQL does", "[copy]") {
	CopyFromRules rules;
	rules.options.format = CopyFormat::CSV;
	rules.options.delimiter = ',';
	rules.options.null_str = "";
	rules.options.has_default = true;
	rules.options.default_str = "D";
	rules.force_not_null = {true, false};
	rules.force_null = {false, true};
	string out(16, '\0');
	std::string_view value;
	REQUIRE(ReadCopyField(rules, 0, "", &out[0], value) == CopyFieldKind::VALUE);
	REQUIRE(ReadCopyField(rules, 1, "", &out[0], value) == CopyFieldKind::NULL_VALUE);
	REQUIRE(ReadCopyField(rules, 0, "\"\"", &out[0], value) == CopyFieldKind::VALUE);
	REQUIRE(ReadCopyField(rules, 1, "\"\"", &out[0], value) == CopyFieldKind::NULL_VALUE);
	REQUIRE(ReadCopyField(rules, 1, "D", &out[0], value) == CopyFieldKind::DEFAULT_VALUE);
	REQUIRE(ReadCopyField(rules, 1, "\"D\"", &out[0], value) == CopyFieldKind::VALUE);
	REQUIRE(value == "D");
	rules.options.format = CopyFormat::TEXT;
	rules.options.null_str = "\\N";
	REQUIRE(ReadCopyField(rules, 0, "\\N", &out[0], value) == CopyFieldKind::NULL_VALUE);
	REQUIRE(ReadCopyField(rules, 0, "\\\\N", &out[0], value) == CopyFieldKind::VALUE);
	REQUIRE(value == "\\N");
}

namespace {

string EncodeInChunks(TextEncoding encoding, std::string_view utf8, idx_t chunk) {
	CopyEncoder encoder(encoding);
	string out;
	for (idx_t at = 0; at < utf8.size(); at += chunk) {
		encoder.Encode(utf8.substr(at, chunk), out);
	}
	return out;
}

template <class F>
string ErrorOf(F &&f) {
	try {
		f();
	} catch (const std::exception &ex) {
		return ErrorData(ex).RawMessage();
	}
	return "no error";
}

} // namespace

TEST_CASE("COPY encodes its output in every encoding, however the output is split", "[copy]") {
	const std::string_view text("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z");
	const vector<std::pair<TextEncoding, std::string_view>> encoded = {
	    {TextEncoding::UTF16_LE, std::string_view("\x61\x00\xE9\x00\xAC\x20\x3D\xD8\x00\xDE\x7A\x00", 12)},
	    {TextEncoding::UTF16_BE, std::string_view("\x00\x61\x00\xE9\x20\xAC\xD8\x3D\xDE\x00\x00\x7A", 12)},
	    {TextEncoding::UTF32_LE,
	     std::string_view("\x61\x00\x00\x00\xE9\x00\x00\x00\xAC\x20\x00\x00\x00\xF6\x01\x00\x7A\x00\x00\x00", 20)},
	    {TextEncoding::UTF32_BE,
	     std::string_view("\x00\x00\x00\x61\x00\x00\x00\xE9\x00\x00\x20\xAC\x00\x01\xF6\x00\x00\x00\x00\x7A", 20)},
	};
	for (auto &entry : encoded) {
		for (idx_t chunk = 1; chunk <= text.size(); chunk++) {
			INFO("encoding " << static_cast<int>(entry.first) << " chunk " << chunk);
			REQUIRE(EncodeInChunks(entry.first, text, chunk) == string(entry.second));
		}
	}
	const std::string_view utf8("caf\xC3\xA9 na\xC3\xAFve \xC3\xBF");
	for (idx_t chunk = 1; chunk <= utf8.size(); chunk++) {
		REQUIRE(EncodeInChunks(TextEncoding::LATIN1, utf8, chunk) == "caf\xE9 na\xEFve \xFF");
	}
}

TEST_CASE("COPY names the character its encoding cannot hold, and invalid UTF-8, as PostgreSQL does", "[copy]") {
	const auto latin1 = [](std::string_view utf8, idx_t chunk) {
		return ErrorOf([&] { EncodeInChunks(TextEncoding::LATIN1, utf8, chunk); });
	};
	REQUIRE(
	    latin1("a\xC3\xA9\xE2\x82\xAC", 64) ==
	    "character with byte sequence 0xe2 0x82 0xac in encoding \"UTF8\" has no equivalent in encoding \"LATIN1\"");
	REQUIRE(latin1("\xC3\xBF\xC4\x80", 64) ==
	        "character with byte sequence 0xc4 0x80 in encoding \"UTF8\" has no equivalent in encoding \"LATIN1\"");
	REQUIRE(latin1("\xF0\x9F\x98\x80", 1) == "character with byte sequence 0xf0 0x9f 0x98 0x80 in encoding \"UTF8\" "
	                                         "has no equivalent in encoding \"LATIN1\"");
	REQUIRE(latin1("ab\x80", 64) == "invalid byte sequence for encoding \"UTF8\": 0x80");
	REQUIRE(ErrorOf([] { EncodeInChunks(TextEncoding::UTF16_LE, "ab\x80", 64); }) ==
	        "invalid byte sequence for encoding \"UTF8\": 0x80");
}

TEST_CASE("COPY reads PostgreSQL's encoding names", "[copy]") {
	const vector<std::pair<string, string>> names = {
	    {"UTF8", "utf-8"},        {"utf-8", "utf-8"},        {"Unicode", "utf-8"},    {"SQL_ASCII", "utf-8"},
	    {"LATIN1", "latin-1"},    {"ISO_8859_1", "latin-1"}, {"UTF16", "utf-16le"},   {"utf-16le", "utf-16le"},
	    {"UTF-16BE", "utf-16be"}, {"utf32", "utf-32le"},     {"UTF32LE", "utf-32le"}, {"utf_32_be", "utf-32be"},
	};
	for (auto &name : names) {
		REQUIRE(CopyEncodingName(name.first) == name.second);
		REQUIRE(CopyEncodingTitle(name.second) == StringUtil::Upper(StringUtil::Replace(name.second, "-", "")));
	}
	REQUIRE(ErrorOf([] { CopyEncodingName("WIN1252"); }) == "COPY ENCODING \"WIN1252\" is not supported");
	REQUIRE(ErrorOf([] { CopyEncodingName("nope"); }) ==
	        "argument to option \"encoding\" must be a valid encoding name");
	REQUIRE(!CopyOutputEncoding("utf-8"));
	REQUIRE(*CopyOutputEncoding("latin-1") == TextEncoding::LATIN1);
	REQUIRE(*CopyOutputEncoding("utf-16be") == TextEncoding::UTF16_BE);
	REQUIRE(*CopyOutputEncoding("utf-32le") == TextEncoding::UTF32_LE);
}

TEST_CASE("COPY's text format reads hex escapes of either case and stops at the first byte that is no digit",
          "[copy]") {
	string out(64, '\0');
	REQUIRE(UnescapePostgresText("\\x!\\xab\\xAB\\xfg\\x4", &out[0]) == string("x!\xAB\xAB\x0F") + "g\x04");
	REQUIRE(UnescapePostgresText("\\1!", &out[0]) == "\x01!");
}

TEST_CASE("COPY's line break finder steps over the byte an escape at the end of a piece escapes", "[copy]") {
	CopyFormatOptions options;
	options.format = CopyFormat::CSV;
	options.escape = '\\';
	for (const string second : {"\\b\"\r", "nb\"\r"}) {
		INFO("second piece " << Escaped(second));
		CopyLineBreakFinder finder(options);
		REQUIRE(finder.Feed("\"a\\") == '\0');
		REQUIRE(finder.Feed(second) == '\0');
		REQUIRE(finder.Finish() == '\r');
	}
}

TEST_CASE("COPY's printout of a long value ends at a character start, even with none in its first bytes", "[copy]") {
	REQUIRE(CopyPrintout("a" + string(150, '\x80')) == "...");
	REQUIRE(CopyPrintout(string(99, 'a') + "\xC3\xA9z") == string(99, 'a') + "...");
}

TEST_CASE("COPY's encoder names invalid UTF-8 however many continuation bytes end its input", "[copy]") {
	CopyEncoder encoder(TextEncoding::LATIN1);
	string out;
	encoder.Encode("", out);
	REQUIRE(out.empty());
	REQUIRE(ErrorOf([] { EncodeInChunks(TextEncoding::LATIN1, "\x80", 1); }) ==
	        "invalid byte sequence for encoding \"UTF8\": 0x80");
	REQUIRE(ErrorOf([] { EncodeInChunks(TextEncoding::UTF16_LE, "a\x80\x80\x80\x80", 64); }) ==
	        "invalid byte sequence for encoding \"UTF8\": 0x80");
	REQUIRE(ErrorOf([] { EncodeInChunks(TextEncoding::UTF32_LE, "ab\x80", 64); }) ==
	        "invalid byte sequence for encoding \"UTF8\": 0x80");
}

TEST_CASE("COPY in binary mode refuses the options of the text formats under every spelling", "[copy]") {
	const auto refused = [](const char *key) {
		identifier_map_t<vector<Value>> options;
		options[key] = {Value("x")};
		return ErrorOf([&] { ParseCopyFormatOptions(CopyFormat::BINARY, true, options); });
	};
	for (const auto key : {"delim", "sep", "separator"}) {
		REQUIRE(refused(key) == "cannot specify DELIMITER in BINARY mode");
	}
	REQUIRE(refused("nullstr") == "cannot specify NULL in BINARY mode");
	REQUIRE(refused("escape") == "COPY ESCAPE requires CSV mode");
	REQUIRE(refused("force_quote") == "COPY FORCE_QUOTE requires CSV mode");
	REQUIRE(refused("force_not_null") == "COPY FORCE_NOT_NULL requires CSV mode");
	REQUIRE(refused("force_null") == "COPY FORCE_NULL requires CSV mode");
}
