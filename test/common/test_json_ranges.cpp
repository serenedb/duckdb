#include "catch.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/formats/json_ranges.hpp"
#include "duckdb/formats/row_chain.hpp"

#include <random>

using namespace duckdb;

namespace {

struct Walker {
	bool in_string = false;
	bool escape = false;
	int64_t depth = 0;

	void Step(char c) {
		const bool escaped = escape;
		escape = false;
		if (!escaped && c == '\\') {
			escape = true;
			return;
		}
		if (!escaped && c == '"') {
			in_string = !in_string;
			return;
		}
		if (in_string) {
			return;
		}
		if (c == '{' || c == '[') {
			depth++;
		} else if (c == '}' || c == ']') {
			depth--;
		}
	}

	JsonRangeOrigin Origin() const {
		if (!in_string) {
			return JsonRangeOrigin::OUTSIDE;
		}
		return escape ? JsonRangeOrigin::ESCAPED : JsonRangeOrigin::STRING;
	}
};

idx_t SkipLeading(const string &text, idx_t position, idx_t end, bool file_start) {
	if (file_start && end - position >= 3 && text.compare(position, 3, "\xEF\xBB\xBF") == 0) {
		position += 3;
	}
	while (position < end && StringUtil::CharacterIsSpace(text[position])) {
		position++;
	}
	return position;
}

JsonRangeStart ReferenceTransfer(const string &text, idx_t begin, idx_t end, JsonRangeOrigin origin, bool file_start) {
	Walker walker;
	idx_t position = begin;
	if (origin == JsonRangeOrigin::BEFORE_ARRAY) {
		position = SkipLeading(text, begin, end, file_start);
		if (position == end) {
			return JsonRangeStart {JsonRangeOrigin::BEFORE_ARRAY, 0};
		}
	} else {
		walker.in_string = origin != JsonRangeOrigin::OUTSIDE;
		walker.escape = origin == JsonRangeOrigin::ESCAPED;
	}
	for (; position < end; position++) {
		walker.Step(text[position]);
	}
	return JsonRangeStart {walker.Origin(), walker.depth};
}

struct Document {
	string text;
	idx_t open = 0;
	vector<JsonRangeStart> states;
	vector<idx_t> separators;
	bool bom = false;
};

void AddString(std::mt19937_64 &rng, string &out) {
	out += '"';
	const idx_t pieces = rng() % 12;
	for (idx_t i = 0; i < pieces; i++) {
		switch (rng() % 12) {
		case 0:
			out += "\\\"";
			break;
		case 1:
			out += string(2 * (rng() % 70), '\\');
			break;
		case 2:
			out += "\\\\\\\"";
			break;
		case 3:
			out += "[{";
			break;
		case 4:
			out += "}]";
			break;
		case 5:
			out += ", ";
			break;
		case 6:
			out += "\\n\\u00e9";
			break;
		case 7:
			out += "\xC3\xA9:";
			break;
		default:
			out += string(1 + rng() % 40, static_cast<char>('a' + rng() % 26));
			break;
		}
	}
	out += '"';
}

void AddSpace(std::mt19937_64 &rng, string &out, bool pretty) {
	if (pretty && rng() % 2 == 0) {
		out += "\n";
		out += string(rng() % 4, '\t');
	} else if (rng() % 4 == 0) {
		out += ' ';
	}
}

void AddValue(std::mt19937_64 &rng, string &out, idx_t depth, bool pretty) {
	switch (rng() % (depth > 3 ? 4 : 7)) {
	case 0:
		out += std::to_string(static_cast<int64_t>(rng() % 200000) - 100000);
		break;
	case 1:
	case 3:
		AddString(rng, out);
		break;
	case 2:
		out += rng() % 2 ? "true" : "null";
		break;
	case 4:
	case 6: {
		out += '{';
		const idx_t members = rng() % 5;
		for (idx_t i = 0; i < members; i++) {
			if (i) {
				out += ',';
			}
			AddSpace(rng, out, pretty);
			AddString(rng, out);
			out += ':';
			AddValue(rng, out, depth + 1, pretty);
		}
		AddSpace(rng, out, pretty);
		out += '}';
		break;
	}
	default: {
		out += '[';
		const idx_t items = rng() % 5;
		for (idx_t i = 0; i < items; i++) {
			if (i) {
				out += ',';
			}
			AddSpace(rng, out, pretty);
			AddValue(rng, out, depth + 1, pretty);
		}
		out += ']';
		break;
	}
	}
}

Document MakeDocument(std::mt19937_64 &rng, idx_t elements) {
	Document document;
	const bool pretty = rng() % 2 == 0;
	document.bom = rng() % 4 == 0;
	auto &text = document.text;
	if (document.bom) {
		text += "\xEF\xBB\xBF";
	}
	text += string(rng() % 3, ' ');
	if (rng() % 3 == 0) {
		text += "\n\t";
	}
	document.open = text.size();
	text += '[';
	for (idx_t i = 0; i < elements; i++) {
		if (i) {
			text += ',';
		}
		AddSpace(rng, text, pretty);
		AddValue(rng, text, 1, pretty);
	}
	AddSpace(rng, text, pretty);
	text += ']';
	text += string(rng() % 3, '\n');
	Walker walker;
	document.states.resize(text.size() + 1);
	for (idx_t position = 0; position <= text.size(); position++) {
		if (position <= document.open) {
			document.states[position] = JsonRangeStart {JsonRangeOrigin::BEFORE_ARRAY, 0};
		} else {
			document.states[position] = JsonRangeStart {walker.Origin(), walker.depth};
		}
		if (position == text.size()) {
			break;
		}
		if (position >= document.open) {
			if (text[position] == ',' && !walker.in_string && !walker.escape && walker.depth == 1) {
				document.separators.push_back(position);
			}
			walker.Step(text[position]);
		}
	}
	REQUIRE(walker.depth == 0);
	REQUIRE(!walker.in_string);
	return document;
}

Document MakeValues(std::mt19937_64 &rng, idx_t values) {
	Document document;
	const bool pretty = rng() % 2 == 0;
	document.bom = rng() % 4 == 0;
	auto &text = document.text;
	if (document.bom) {
		text += "\xEF\xBB\xBF";
	}
	text += string(rng() % 3, ' ');
	for (idx_t i = 0; i < values; i++) {
		if (i) {
			text += rng() % 2 ? "\n" : " ";
		}
		AddSpace(rng, text, pretty);
		switch (rng() % 4) {
		case 0:
			AddString(rng, text);
			break;
		case 1: {
			text += '[';
			AddValue(rng, text, 1, pretty);
			text += ']';
			break;
		}
		default: {
			text += '{';
			AddString(rng, text);
			text += ':';
			AddValue(rng, text, 1, pretty);
			AddSpace(rng, text, pretty);
			text += '}';
			break;
		}
		}
	}
	text += string(rng() % 3, '\n');
	Walker walker;
	document.states.resize(text.size() + 1);
	for (idx_t position = 0; position <= text.size(); position++) {
		document.states[position] = JsonRangeStart {walker.Origin(), walker.depth};
		if (position == text.size()) {
			break;
		}
		const char c = text[position];
		if (!walker.in_string && !walker.escape && walker.depth == 0 && (c == '{' || c == '[' || c == '"')) {
			document.separators.push_back(position);
		}
		walker.Step(c);
	}
	REQUIRE(walker.depth == 0);
	REQUIRE(!walker.in_string);
	REQUIRE(document.separators.size() == values);
	return document;
}

idx_t ReferenceSeparator(const Document &document, idx_t begin, idx_t end) {
	for (auto separator : document.separators) {
		if (separator >= begin && separator < end) {
			return separator - begin;
		}
	}
	return DConstants::INVALID_INDEX;
}

bool SameStart(const JsonRangeStart &left, const JsonRangeStart &right) {
	return left.origin == right.origin && left.depth == right.depth;
}

bool InsideBom(const Document &document, idx_t position) {
	return document.bom && position > 0 && position < 3;
}

using JsonChain = RowChain<JsonRangeStart, JsonRangeEntry, JSON_RANGE_ORIGINS>;

} // namespace

TEST_CASE("JSON range transfers follow the bytes from every origin", "[formats][json]") {
	std::mt19937_64 rng(20261121);
	for (idx_t round = 0; round < 60; round++) {
		const auto document = MakeDocument(rng, 1 + rng() % 12);
		const auto &text = document.text;
		const idx_t size = text.size();
		for (idx_t begin = 0; begin <= size; begin++) {
			if (InsideBom(document, begin)) {
				continue;
			}
			const idx_t ends[] = {size, MinValue<idx_t>(size, begin + rng() % 200), MinValue<idx_t>(size, begin + 64),
			                      MaxValue<idx_t>(begin, document.open)};
			for (auto end : ends) {
				if (InsideBom(document, end)) {
					continue;
				}
				const char *data = text.data() + begin;
				const idx_t span = end - begin;
				const bool file_start = begin == 0;
				const auto truth = document.states[begin];
				const auto expected = ReferenceTransfer(text, begin, end, truth.origin, file_start);
				const auto entry = JsonRangeTransfer(data, span, truth.origin, file_start);
				REQUIRE(entry.computed);
				REQUIRE(SameStart(entry.end, expected));
				REQUIRE(SameStart(truth.Next(entry), document.states[end]));
				JsonRangeEntry outside;
				JsonRangeEntry string;
				JsonRangeTransfers(data, span, outside, string);
				REQUIRE(SameStart(outside.end, ReferenceTransfer(text, begin, end, JsonRangeOrigin::OUTSIDE, false)));
				REQUIRE(SameStart(string.end, ReferenceTransfer(text, begin, end, JsonRangeOrigin::STRING, false)));
				const auto escaped = JsonRangeTransfer(data, span, JsonRangeOrigin::ESCAPED, false);
				REQUIRE(SameStart(escaped.end, ReferenceTransfer(text, begin, end, JsonRangeOrigin::ESCAPED, false)));
				if (begin != 0) {
					REQUIRE(JsonArraySeparator(data, span, truth) == ReferenceSeparator(document, begin, end));
				}
			}
		}
	}
}

TEST_CASE("A JSON array range that starts before an opening that is not a bracket owns no element", "[formats][json]") {
	const string text = "  {\"a\": 1}, [2]";
	const JsonRangeStart start {JsonRangeOrigin::BEFORE_ARRAY, 0};
	REQUIRE(JsonArraySeparator(text.data(), text.size(), start) == DConstants::INVALID_INDEX);
	REQUIRE(JsonArraySeparator(text.data(), 2, start) == DConstants::INVALID_INDEX);
}

TEST_CASE("JSON array ranges resolve their starts through the chain and own disjoint elements", "[formats][json]") {
	std::mt19937_64 rng(1121);
	for (idx_t round = 0; round < 200; round++) {
		const auto document = MakeDocument(rng, 1 + rng() % 40);
		const auto &text = document.text;
		const idx_t size = text.size();
		const idx_t range_bytes = MaxValue<idx_t>(3, size / (1 + rng() % 128) + rng() % 3);
		const idx_t count = (size + range_bytes - 1) / range_bytes;
		const auto range_begin = [&](idx_t range) {
			return MinValue(range * range_bytes, size);
		};
		const auto range_end = [&](idx_t range) {
			return MinValue((range + 1) * range_bytes, size);
		};
		mutex lock;
		JsonChain chain;
		chain.Assign(count, JsonRangeStart());
		for (idx_t range = 0; range < count; range++) {
			const idx_t begin = range_begin(range);
			if (range == 0) {
				chain.Publish(range, static_cast<uint8_t>(JsonRangeOrigin::BEFORE_ARRAY),
				              JsonRangeTransfer(text.data(), range_end(0), JsonRangeOrigin::BEFORE_ARRAY, true), count);
				continue;
			}
			JsonRangeEntry outside;
			JsonRangeEntry string;
			JsonRangeTransfers(text.data() + begin, range_end(range) - begin, outside, string);
			chain.Publish(range, static_cast<uint8_t>(JsonRangeOrigin::OUTSIDE), outside, count);
			chain.Publish(range, static_cast<uint8_t>(JsonRangeOrigin::STRING), string, count);
		}
		vector<idx_t> owned;
		for (idx_t range = 1; range < count; range++) {
			const idx_t begin = range_begin(range);
			const auto start = chain.StartState(
			    lock, range,
			    [&](idx_t missing, uint8_t origin) {
				    const idx_t from = range_begin(missing);
				    return JsonRangeTransfer(text.data() + from, range_end(missing) - from,
				                             static_cast<JsonRangeOrigin>(origin), missing == 0);
			    },
			    [&](idx_t missing, uint8_t origin, const JsonRangeEntry &entry) {
				    chain.Publish(missing, origin, entry, count);
			    });
			REQUIRE(SameStart(start, document.states[begin]));
			const idx_t separator = JsonArraySeparator(text.data() + begin, range_end(range) - begin, start);
			REQUIRE(separator == ReferenceSeparator(document, begin, range_end(range)));
			if (separator != DConstants::INVALID_INDEX) {
				owned.push_back(begin + separator);
			}
		}
		for (auto separator : owned) {
			bool found = false;
			for (auto expected : document.separators) {
				found = found || expected == separator;
			}
			REQUIRE(found);
		}
	}
}

TEST_CASE("JSON value starts of unstructured files follow the top-level values from every split", "[formats][json]") {
	std::mt19937_64 rng(2112);
	for (idx_t round = 0; round < 60; round++) {
		const auto document = MakeValues(rng, 1 + rng() % 12);
		const auto &text = document.text;
		const idx_t size = text.size();
		for (idx_t begin = 0; begin <= size; begin++) {
			const idx_t ends[] = {size, MinValue<idx_t>(size, begin + rng() % 200), MinValue<idx_t>(size, begin + 64)};
			for (auto end : ends) {
				const auto truth = document.states[begin];
				REQUIRE(JsonValueStart(text.data() + begin, end - begin, truth) ==
				        ReferenceSeparator(document, begin, end));
			}
		}
	}
}

TEST_CASE("JSON unstructured ranges resolve their starts through the chain", "[formats][json]") {
	std::mt19937_64 rng(2111);
	for (idx_t round = 0; round < 200; round++) {
		const auto document = MakeValues(rng, 1 + rng() % 40);
		const auto &text = document.text;
		const idx_t size = text.size();
		const idx_t range_bytes = MaxValue<idx_t>(3, size / (1 + rng() % 128) + rng() % 3);
		const idx_t count = (size + range_bytes - 1) / range_bytes;
		const auto range_begin = [&](idx_t range) {
			return MinValue(range * range_bytes, size);
		};
		const auto range_end = [&](idx_t range) {
			return MinValue((range + 1) * range_bytes, size);
		};
		mutex lock;
		JsonChain chain;
		chain.Assign(count, JsonRangeStart {JsonRangeOrigin::OUTSIDE, 0});
		for (idx_t range = 0; range < count; range++) {
			const idx_t begin = range_begin(range);
			JsonRangeEntry outside;
			JsonRangeEntry string;
			JsonRangeTransfers(text.data() + begin, range_end(range) - begin, outside, string);
			chain.Publish(range, static_cast<uint8_t>(JsonRangeOrigin::OUTSIDE), outside, count);
			chain.Publish(range, static_cast<uint8_t>(JsonRangeOrigin::STRING), string, count);
		}
		for (idx_t range = 1; range < count; range++) {
			const idx_t begin = range_begin(range);
			const auto start = chain.StartState(
			    lock, range,
			    [&](idx_t missing, uint8_t origin) {
				    const idx_t from = range_begin(missing);
				    return JsonRangeTransfer(text.data() + from, range_end(missing) - from,
				                             static_cast<JsonRangeOrigin>(origin), false);
			    },
			    [&](idx_t missing, uint8_t origin, const JsonRangeEntry &entry) {
				    chain.Publish(missing, origin, entry, count);
			    });
			REQUIRE(SameStart(start, document.states[begin]));
			REQUIRE(JsonValueStart(text.data() + begin, range_end(range) - begin, start) ==
			        ReferenceSeparator(document, begin, range_end(range)));
		}
	}
}
