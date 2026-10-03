#include "unicode_properties.hpp"

#include "property_data.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

namespace duckdb {
namespace text {

namespace {

constexpr uint32_t BINARY_LIMIT = 0x1000;
constexpr uint32_t INT_START = 0x1000;
constexpr uint32_t INT_LIMIT = 0x2000;
constexpr uint32_t CANONICAL_COMBINING_CLASS = 0x1002;
constexpr uint32_t GENERAL_CATEGORY = 0x1005;
constexpr uint32_t SCRIPT = 0x100A;
constexpr uint32_t LEAD_CANONICAL_COMBINING_CLASS = 0x1010;
constexpr uint32_t TRAIL_CANONICAL_COMBINING_CLASS = 0x1011;
constexpr uint32_t GENERAL_CATEGORY_MASK = 0x2000;
constexpr uint32_t MASK_LIMIT = 0x2001;
constexpr uint32_t NUMERIC_VALUE = 0x3000;
constexpr uint32_t AGE = 0x4000;
constexpr uint32_t SCRIPT_EXTENSIONS = 0x7000;
constexpr uint32_t IDENTIFIER_TYPE = 0x7001;
constexpr int64_t INVALID = -1;
constexpr uint32_t CODE_POINT_LIMIT = 0x110000;
constexpr uint32_t VALUES_ARRAY_COUNT = 9;
constexpr size_t MAX_MUNGED_NAME = 127;

constexpr uint32_t INVARIANT_CHARS[4] = {0xfffffbff, 0xffffffe5, 0x87fffffe, 0x87fffffe};

struct PropertyRecord {
	uint32_t number;
	uint32_t first_run;
	uint32_t run_count;
	uint32_t value_map;
};

struct ValueMapRecord {
	uint32_t property;
	uint32_t first_alias;
	uint32_t alias_count;
};

struct AliasRecord {
	uint32_t offset;
	uint32_t length;
	uint32_t value;
};

struct PropertyData {
	const double *numeric_values;
	const uint32_t *run_starts;
	const PropertyRecord *properties;
	uint32_t property_count;
	const ValueMapRecord *value_maps;
	uint32_t value_map_count;
	const AliasRecord *property_aliases;
	uint32_t property_alias_count;
	const AliasRecord *value_aliases;
	const uint16_t *run_values;
	const uint16_t *value_lists;
	const char *chars;
};

template <class T>
const T *ReadRecords(TextUnitReader &reader, uint32_t &count) {
	auto result = reinterpret_cast<const T *>(reader.Read<uint32_t>(count));
	count /= sizeof(T) / sizeof(uint32_t);
	return result;
}

const PropertyData &GetData() {
	static const PropertyData data = []() {
		TextUnitReader reader(LoadUnit(property_units[property_values_unit]), VALUES_ARRAY_COUNT);
		PropertyData result {};
		uint32_t count;
		result.numeric_values = reader.Read<double>();
		result.run_starts = reader.Read<uint32_t>();
		result.properties = ReadRecords<PropertyRecord>(reader, result.property_count);
		result.value_maps = ReadRecords<ValueMapRecord>(reader, result.value_map_count);
		result.property_aliases = ReadRecords<AliasRecord>(reader, result.property_alias_count);
		result.value_aliases = ReadRecords<AliasRecord>(reader, count);
		result.run_values = reader.Read<uint16_t>();
		result.value_lists = reader.Read<uint16_t>();
		result.chars = reinterpret_cast<const char *>(reader.Read<uint8_t>());
		return result;
	}();
	return data;
}

bool IsInvariant(char c) {
	auto byte = static_cast<uint8_t>(c);
	return byte <= 0x7F && (INVARIANT_CHARS[byte >> 5] & (1u << (byte & 0x1F))) != 0;
}

std::string_view TruncateAtNul(std::string_view text) {
	return text.substr(0, std::min(text.find('\0'), text.size()));
}

std::string LooseName(std::string_view name) {
	std::string result;
	for (auto c : name) {
		if (c == '-' || c == '_' || c == ' ' || (c >= '\t' && c <= '\r')) {
			continue;
		}
		result.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c);
	}
	return result;
}

int64_t FindAlias(const PropertyData &data, const AliasRecord *aliases, uint32_t count, std::string_view name) {
	auto key = LooseName(name);
	if (key.empty()) {
		return INVALID;
	}
	auto end = aliases + count;
	auto entry = std::lower_bound(aliases, end, key, [&](const AliasRecord &alias, const std::string &value) {
		return std::string_view(data.chars + alias.offset, alias.length) < value;
	});
	if (entry == end || std::string_view(data.chars + entry->offset, entry->length) != key) {
		return INVALID;
	}
	return entry->value;
}

int64_t FindProperty(const PropertyData &data, std::string_view name) {
	return FindAlias(data, data.property_aliases, data.property_alias_count, name);
}

int64_t FindValue(const PropertyData &data, uint32_t property, std::string_view name) {
	for (uint32_t i = 0; i < data.value_map_count; i++) {
		auto &map = data.value_maps[i];
		if (map.property == property) {
			return FindAlias(data, data.value_aliases + map.first_alias, map.alias_count, name);
		}
	}
	return INVALID;
}

const PropertyRecord *FindRecord(const PropertyData &data, uint32_t property) {
	for (uint32_t i = 0; i < data.property_count; i++) {
		if (data.properties[i].number == property) {
			return &data.properties[i];
		}
	}
	return nullptr;
}

bool LooseEquals(std::string_view left, std::string_view right) {
	return LooseName(left) == LooseName(right);
}

void AddRange(std::vector<PropertyRange> &ranges, uint32_t first, uint32_t last) {
	if (!ranges.empty() && ranges.back().last + 1 == first) {
		ranges.back().last = last;
	} else {
		ranges.push_back({first, last});
	}
}

template <class PREDICATE>
void CollectRuns(const PropertyData &data, const PropertyRecord &record, std::vector<PropertyRange> &ranges,
                 PREDICATE &&predicate) {
	for (uint32_t i = 0; i < record.run_count; i++) {
		auto run = record.first_run + i;
		if (!predicate(data.run_values[run])) {
			continue;
		}
		auto last = i + 1 < record.run_count ? data.run_starts[run + 1] - 1 : CODE_POINT_LIMIT - 1;
		AddRange(ranges, data.run_starts[run], last);
	}
}

bool ListContains(const PropertyData &data, uint16_t list, int64_t value) {
	auto count = data.value_lists[list];
	for (uint16_t i = 1; i <= count; i++) {
		if (data.value_lists[list + i] == value) {
			return true;
		}
	}
	return false;
}

void Complement(std::vector<PropertyRange> &ranges) {
	std::vector<PropertyRange> result;
	uint32_t next = 0;
	for (auto &range : ranges) {
		if (range.first > next) {
			result.push_back({next, range.first - 1});
		}
		next = range.last + 1;
	}
	if (next < CODE_POINT_LIMIT) {
		result.push_back({next, CODE_POINT_LIMIT - 1});
	}
	ranges = std::move(result);
}

bool ApplyValue(const PropertyData &data, uint32_t property, int64_t value, std::vector<PropertyRange> &ranges) {
	auto record = FindRecord(data, property == GENERAL_CATEGORY_MASK ? GENERAL_CATEGORY : property);
	if (!record) {
		return false;
	}
	if (property == GENERAL_CATEGORY_MASK) {
		CollectRuns(data, *record, ranges, [&](uint16_t gc) { return ((uint64_t(1) << gc) & uint64_t(value)) != 0; });
		return true;
	}
	if (property == SCRIPT_EXTENSIONS || property == IDENTIFIER_TYPE) {
		CollectRuns(data, *record, ranges, [&](uint16_t list) { return ListContains(data, list, value); });
		return true;
	}
	if (property < BINARY_LIMIT) {
		if (value == 0 || value == 1) {
			CollectRuns(data, *record, ranges, [&](uint16_t member) { return member == value; });
		}
		return true;
	}
	if (property >= INT_START && property < INT_LIMIT) {
		CollectRuns(data, *record, ranges, [&](uint16_t member) { return member == value; });
		return true;
	}
	return false;
}

std::string MungeName(std::string_view name, bool &fits) {
	std::string result;
	for (auto c : name) {
		if (c == ' ' && (result.empty() || result.back() == ' ')) {
			continue;
		}
		if (result.size() >= MAX_MUNGED_NAME) {
			fits = false;
			return result;
		}
		result.push_back(c);
	}
	if (!result.empty() && result.back() == ' ') {
		result.pop_back();
	}
	fits = true;
	return result;
}

bool ParseNumber(std::string_view text, double &value) {
	std::string copy(text);
	char *end;
	value = std::strtod(copy.c_str(), &end);
	return *end == '\0';
}

void ParseVersion(const std::string &text, uint8_t version[4]) {
	const char *position = text.c_str();
	uint32_t part = 0;
	for (;;) {
		char *end;
		version[part] = static_cast<uint8_t>(std::strtoul(position, &end, 10));
		if (end == position || ++part == 4 || *end != '.') {
			break;
		}
		position = end + 1;
	}
	while (part < 4) {
		version[part++] = 0;
	}
}

bool ApplyAge(const PropertyData &data, std::string_view value, std::vector<PropertyRange> &ranges) {
	bool fits;
	auto munged = MungeName(value, fits);
	if (!fits) {
		return false;
	}
	uint8_t version[4];
	ParseVersion(munged, version);
	auto record = FindRecord(data, AGE);
	if (!record) {
		return false;
	}
	CollectRuns(data, *record, ranges, [&](uint16_t age) {
		if (age == 0) {
			return false;
		}
		uint8_t of_character[4] = {uint8_t(age >> 8), uint8_t(age & 0xFF), 0, 0};
		return std::memcmp(of_character, version, 4) <= 0;
	});
	return true;
}

bool ApplyWithValue(const PropertyData &data, std::string_view prop, std::string_view value,
                    std::vector<PropertyRange> &ranges) {
	auto property = FindProperty(data, prop);
	if (property == INVALID) {
		return false;
	}
	auto number = static_cast<uint32_t>(property);
	if (number == GENERAL_CATEGORY) {
		number = GENERAL_CATEGORY_MASK;
	}
	if (number < BINARY_LIMIT || (number >= INT_START && number < INT_LIMIT) ||
	    (number >= GENERAL_CATEGORY_MASK && number < MASK_LIMIT)) {
		auto found = FindValue(data, number, value);
		if (found == INVALID) {
			if (number != CANONICAL_COMBINING_CLASS && number != LEAD_CANONICAL_COMBINING_CLASS &&
			    number != TRAIL_CANONICAL_COMBINING_CLASS) {
				return false;
			}
			double numeric;
			if (!ParseNumber(value, numeric) || !(numeric >= 0 && numeric <= 255) ||
			    static_cast<double>(static_cast<int32_t>(numeric)) != numeric) {
				return false;
			}
			found = static_cast<int32_t>(numeric);
		}
		return ApplyValue(data, number, found, ranges);
	}
	switch (number) {
	case NUMERIC_VALUE: {
		double numeric;
		if (!ParseNumber(value, numeric)) {
			return false;
		}
		auto record = FindRecord(data, NUMERIC_VALUE);
		if (!record) {
			return false;
		}
		CollectRuns(data, *record, ranges, [&](uint16_t index) { return data.numeric_values[index] == numeric; });
		return true;
	}
	case AGE:
		return ApplyAge(data, value, ranges);
	case SCRIPT_EXTENSIONS: {
		auto found = FindValue(data, SCRIPT, value);
		return found != INVALID && ApplyValue(data, SCRIPT_EXTENSIONS, found, ranges);
	}
	case IDENTIFIER_TYPE: {
		auto found = FindValue(data, IDENTIFIER_TYPE, value);
		return found != INVALID && ApplyValue(data, IDENTIFIER_TYPE, found, ranges);
	}
	default:
		return false;
	}
}

bool ApplyWithoutValue(const PropertyData &data, std::string_view name, std::vector<PropertyRange> &ranges) {
	auto value = FindValue(data, GENERAL_CATEGORY_MASK, name);
	if (value != INVALID) {
		return ApplyValue(data, GENERAL_CATEGORY_MASK, value, ranges);
	}
	value = FindValue(data, SCRIPT, name);
	if (value != INVALID) {
		return ApplyValue(data, SCRIPT, value, ranges);
	}
	auto property = FindProperty(data, name);
	if (property != INVALID && property < BINARY_LIMIT) {
		return ApplyValue(data, static_cast<uint32_t>(property), 1, ranges);
	}
	if (LooseEquals(name, "ANY")) {
		ranges.push_back({0, CODE_POINT_LIMIT - 1});
		return true;
	}
	if (LooseEquals(name, "ASCII")) {
		ranges.push_back({0, 0x7F});
		return true;
	}
	if (LooseEquals(name, "Assigned")) {
		auto unassigned = FindValue(data, GENERAL_CATEGORY_MASK, "Cn");
		ApplyValue(data, GENERAL_CATEGORY_MASK, unassigned, ranges);
		Complement(ranges);
		return true;
	}
	return false;
}

} // namespace

bool UnicodeProperties::Lookup(std::string_view expression, std::vector<PropertyRange> &ranges) {
	ranges.clear();
	for (auto c : expression) {
		if (!IsInvariant(c)) {
			return false;
		}
	}
	auto &data = GetData();
	auto equals = expression.find('=');
	if (equals != std::string_view::npos && equals + 1 < expression.size()) {
		auto prop = TruncateAtNul(expression.substr(0, equals));
		auto value = TruncateAtNul(expression.substr(equals + 1));
		return ApplyWithValue(data, prop, value, ranges);
	}
	auto name = equals == std::string_view::npos ? expression : expression.substr(0, equals);
	return ApplyWithoutValue(data, TruncateAtNul(name), ranges);
}

} // namespace text
} // namespace duckdb
