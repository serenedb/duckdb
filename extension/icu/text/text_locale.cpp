#include "text_locale.hpp"

#include "text_data.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

namespace duckdb {
namespace text {

namespace {

constexpr size_t MAX_LANGUAGE_LENGTH = 11;
constexpr size_t MAX_VARIANTS_LENGTH = 179;
constexpr size_t MAX_CHARSET_LENGTH = 64;

bool IsTerminator(char c) {
	return c == '.' || c == '@';
}

bool IsSeparator(char c) {
	return c == '_' || c == '-';
}

bool IsAsciiLetter(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsAsciiDigit(char c) {
	return c >= '0' && c <= '9';
}

char ToLower(char c) {
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 0x20) : c;
}

char ToUpper(char c) {
	return c >= 'a' && c <= 'z' ? static_cast<char>(c - 0x20) : c;
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (size_t i = 0; i < left.size(); i++) {
		if (ToLower(left[i]) != ToLower(right[i])) {
			return false;
		}
	}
	return true;
}

bool IsBcp47Extension(std::string_view id) {
	return id.size() >= 3 && id[0] == '-' &&
	       (ToLower(id[1]) == 't' || ToLower(id[1]) == 'u' || ToLower(id[1]) == 'x') && id[2] == '-';
}

bool LooksLikeBcp47(std::string_view id) {
	if (id.find('@') != std::string_view::npos) {
		return false;
	}
	size_t shortest = id.size();
	size_t length = 0;
	bool reset = true;
	for (auto c : id) {
		if (!IsSeparator(c)) {
			if (reset) {
				length = 0;
				reset = false;
			}
			length++;
		} else {
			if (length != 0 && length < shortest) {
				shortest = length;
			}
			reset = true;
		}
	}
	return shortest == 1;
}

bool HasBcp47Extension(std::string_view id) {
	if (!LooksLikeBcp47(id)) {
		return false;
	}
	size_t start = 0;
	bool first = true;
	while (start < id.size()) {
		auto end = std::min(id.find_first_of("-_", start), id.size());
		if (!first && end - start == 1) {
			return true;
		}
		first = false;
		start = end + 1;
	}
	return false;
}

std::optional<uint32_t> FindIndex(const char *const *table, uint32_t count, std::string_view code) {
	for (uint32_t i = 0; i < count; i++) {
		if (code == table[i]) {
			return i;
		}
	}
	return std::nullopt;
}

struct ParsedLocale {
	std::string language;
	std::string script;
	std::string region;
	std::string variant;
	std::string charset;
	bool has_charset = false;
	std::string keywords;
	bool posix_modifier = false;
	std::string_view rest;
};

bool ParseVariant(std::string_view id, std::string &variant, size_t &consumed) {
	size_t index = 0;
	bool need_separator = false;
	std::string_view sub = id;
	for (;;) {
		auto next = sub.find_first_of(".@_-");
		bool finished = next == std::string_view::npos || next + 1 == sub.size();
		auto limit = finished ? sub.size() : next;
		index += limit;
		if (index > MAX_VARIANTS_LENGTH) {
			return false;
		}
		if (need_separator) {
			variant.push_back('_');
		}
		need_separator = true;
		for (size_t i = 0; i < limit; i++) {
			variant.push_back(ToUpper(sub[i]));
		}
		if (finished) {
			consumed = index;
			return true;
		}
		sub.remove_prefix(next);
		if (IsTerminator(sub.front()) || IsBcp47Extension(sub)) {
			consumed = index;
			return true;
		}
		sub.remove_prefix(1);
		index++;
	}
}

bool ParseKeywords(std::string_view id, std::string &keywords) {
	std::vector<std::pair<std::string, std::string>> list;
	while (!id.empty()) {
		while (!id.empty() && id.front() == ' ') {
			id.remove_prefix(1);
		}
		if (id.empty()) {
			break;
		}
		auto equals = id.find('=');
		auto semicolon = id.find(';');
		if (equals == std::string_view::npos || (semicolon != std::string_view::npos && semicolon < equals) ||
		    equals == 0) {
			return false;
		}
		std::string key;
		for (size_t i = 0; i < equals; i++) {
			if (id[i] != ' ') {
				key.push_back(ToLower(id[i]));
			}
		}
		auto value_start = equals + 1;
		while (value_start < id.size() && id[value_start] == ' ') {
			value_start++;
		}
		if (value_start == id.size() || value_start == semicolon) {
			return false;
		}
		auto value_end = semicolon == std::string_view::npos ? id.size() : semicolon;
		auto value = id.substr(value_start, value_end - value_start);
		while (!value.empty() && value.back() == ' ') {
			value.remove_suffix(1);
		}
		bool duplicate = std::any_of(list.begin(), list.end(), [&](const auto &entry) { return entry.first == key; });
		if (!duplicate) {
			list.emplace_back(std::move(key), std::string(value));
		}
		id = semicolon == std::string_view::npos ? std::string_view() : id.substr(semicolon + 1);
	}
	std::stable_sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
	for (size_t i = 0; i < list.size(); i++) {
		keywords += list[i].first;
		keywords += '=';
		keywords += list[i].second;
		if (i + 1 < list.size()) {
			keywords += ';';
		}
	}
	return true;
}

bool Parse(std::string_view id, ParsedLocale &result) {
	if (id.size() == 4 && EqualsIgnoreCase(id, "root")) {
		id.remove_prefix(4);
	} else if (id.size() >= 3 && EqualsIgnoreCase(id.substr(0, 3), "und") &&
	           (id.size() == 3 || id[3] == '-' || id[3] == '_' || id[3] == '@')) {
		id.remove_prefix(3);
	}
	size_t length = id.size() >= 2 && IsSeparator(id[1]) && (ToLower(id[0]) == 'i' || ToLower(id[0]) == 'x') ? 2 : 0;
	while (length < id.size() && !IsTerminator(id[length]) && !IsSeparator(id[length])) {
		if (length == MAX_LANGUAGE_LENGTH) {
			return false;
		}
		length++;
	}
	for (size_t i = 0; i < length; i++) {
		result.language.push_back(ToLower(id[i]));
	}
	if (result.language.size() >= 2 && IsSeparator(id[1])) {
		result.language[1] = '-';
	}
	if (length == 3) {
		auto index = FindIndex(iso_languages_3, iso_languages_3_count, result.language);
		if (index) {
			result.language = iso_languages[*index];
		}
	}
	id.remove_prefix(length);

	if (!id.empty() && IsSeparator(id[0])) {
		auto sub = id.substr(1);
		size_t script_length = 0;
		bool too_long = false;
		while (script_length < sub.size() && !IsTerminator(sub[script_length]) && !IsSeparator(sub[script_length]) &&
		       IsAsciiLetter(sub[script_length])) {
			if (script_length == 4) {
				too_long = true;
				break;
			}
			script_length++;
		}
		if (!too_long && script_length == 4) {
			result.script.push_back(ToUpper(sub[0]));
			for (size_t i = 1; i < 4; i++) {
				result.script.push_back(ToLower(sub[i]));
			}
			id.remove_prefix(5);
		}
	}

	bool has_region = false;
	if (!id.empty() && IsSeparator(id[0])) {
		auto sub = id.substr(1);
		size_t region_length = 0;
		bool too_long = false;
		while (region_length < sub.size() && !IsTerminator(sub[region_length]) && !IsSeparator(sub[region_length])) {
			if (region_length == 3) {
				too_long = true;
				break;
			}
			region_length++;
		}
		if (!too_long && region_length >= 2) {
			for (size_t i = 0; i < region_length; i++) {
				result.region.push_back(ToUpper(sub[i]));
			}
			if (region_length == 3) {
				auto index = FindIndex(iso_countries_3, iso_countries_3_count, result.region);
				if (index) {
					result.region = iso_countries[*index];
				}
			}
			has_region = true;
			id.remove_prefix(region_length + 1);
		}
	}

	if (!id.empty() && IsSeparator(id[0]) && !IsBcp47Extension(id)) {
		size_t skip = !has_region && id.size() > 1 && IsSeparator(id[1]) ? 2 : 1;
		size_t consumed = 0;
		if (!ParseVariant(id.substr(skip), result.variant, consumed)) {
			return false;
		}
		if (consumed > 0) {
			id.remove_prefix(skip + consumed);
		} else {
			result.variant.clear();
		}
	}

	if (!id.empty() && id[0] == '.') {
		id.remove_prefix(1);
		auto at = id.find('@');
		auto charset_length = at == std::string_view::npos ? id.size() : at;
		if (charset_length > MAX_CHARSET_LENGTH) {
			return false;
		}
		result.has_charset = true;
		result.charset = std::string(id.substr(0, charset_length));
		id.remove_prefix(charset_length);
	}

	auto at = id.find('@');
	if (at != std::string_view::npos) {
		auto keywords = id.substr(at);
		auto equals = keywords.find('=');
		auto semicolon = keywords.find(';');
		if (equals == std::string_view::npos) {
			result.keywords = std::string(keywords);
			result.posix_modifier = true;
		} else if (semicolon == std::string_view::npos || semicolon > equals) {
			std::string parsed;
			if (!ParseKeywords(keywords.substr(1), parsed)) {
				return false;
			}
			result.keywords = "@" + parsed;
		}
		result.rest = id.substr(0, at);
	} else {
		result.rest = id;
	}
	return true;
}

} // namespace

Locale Locale::FromName(std::string_view id) {
	Locale result;
	if (HasBcp47Extension(id)) {
		return result;
	}
	ParsedLocale parsed;
	if (!Parse(id, parsed)) {
		return result;
	}
	std::string name = parsed.language;
	result.language_length = static_cast<uint16_t>(name.size());
	if (!parsed.script.empty()) {
		name += '_';
		result.script_offset = static_cast<uint16_t>(name.size());
		result.script_length = static_cast<uint16_t>(parsed.script.size());
		name += parsed.script;
	}
	if (!parsed.region.empty()) {
		name += '_';
		result.region_offset = static_cast<uint16_t>(name.size());
		result.region_length = static_cast<uint16_t>(parsed.region.size());
		name += parsed.region;
	}
	if (!parsed.variant.empty()) {
		if (parsed.region.empty()) {
			name += '_';
		}
		name += '_';
		result.variant_offset = static_cast<uint16_t>(name.size());
		result.variant_length = static_cast<uint16_t>(parsed.variant.size());
		name += parsed.variant;
	}
	if (parsed.has_charset) {
		name += '.';
		name += parsed.charset;
	}
	result.base_length = static_cast<uint16_t>(name.size());
	name += parsed.keywords;
	if (parsed.posix_modifier) {
		result.base_length = static_cast<uint16_t>(name.size());
	}
	result.name = std::move(name);
	result.bogus = false;
	return result;
}

bool Locale::TryParse(std::string_view id, Locale &result, std::string &error) {
	auto fail = [&](const char *reason) {
		error = reason;
		return false;
	};
	if (id.empty()) {
		return fail("the locale is empty");
	}
	if (HasBcp47Extension(id)) {
		return fail("BCP 47 extensions are not supported, use the @keyword=value form");
	}
	ParsedLocale parsed;
	if (!Parse(id, parsed)) {
		return fail("it is not a locale identifier");
	}
	if (!parsed.rest.empty()) {
		return fail("it has unexpected text after the identifier");
	}
	auto &language = parsed.language;
	if (language.size() < 2 || language.size() > 3 ||
	    !std::all_of(language.begin(), language.end(), [](char c) { return c >= 'a' && c <= 'z'; })) {
		return fail("the language is not an ISO 639 code");
	}
	auto language_index = FindIndex(iso_languages, iso_languages_current_count, language);
	if (!language_index) {
		return fail("the language is not an ISO 639 code");
	}
	if (!parsed.region.empty()) {
		auto &region = parsed.region;
		bool letters = region.size() == 2 && IsAsciiLetter(region[0]) && IsAsciiLetter(region[1]);
		bool digits = region.size() == 3 && std::all_of(region.begin(), region.end(), IsAsciiDigit);
		if (!digits && !(letters && FindIndex(iso_countries, iso_countries_current_count, region))) {
			return fail("the region is not an ISO 3166 code or a UN M.49 area");
		}
	}
	if (!parsed.variant.empty()) {
		size_t start = 0;
		while (start <= parsed.variant.size()) {
			auto end = parsed.variant.find('_', start);
			if (end == std::string::npos) {
				end = parsed.variant.size();
			}
			auto length = end - start;
			if (length == 0 || length > 8) {
				return fail("a variant is not 1 to 8 letters or digits");
			}
			for (size_t i = start; i < end; i++) {
				if (!IsAsciiLetter(parsed.variant[i]) && !IsAsciiDigit(parsed.variant[i])) {
					return fail("a variant is not 1 to 8 letters or digits");
				}
			}
			start = end + 1;
		}
	}
	if (parsed.has_charset) {
		if (parsed.charset.empty() || !std::all_of(parsed.charset.begin(), parsed.charset.end(), [](char c) {
			    return IsAsciiLetter(c) || IsAsciiDigit(c) || c == '-' || c == '_';
		    })) {
			return fail("the charset is not letters, digits, '-' and '_'");
		}
	}
	if (!parsed.keywords.empty()) {
		auto body = std::string_view(parsed.keywords).substr(1);
		auto valid = [](std::string_view text, bool key) {
			return !text.empty() && std::all_of(text.begin(), text.end(), [&](char c) {
				return (c >= 'a' && c <= 'z') || IsAsciiDigit(c) ||
				       (!key && (IsAsciiLetter(c) || c == '-' || c == '_'));
			});
		};
		if (parsed.posix_modifier) {
			if (!valid(body, false)) {
				return fail("the modifier is not letters and digits");
			}
		} else {
			size_t start = 0;
			while (start < body.size()) {
				auto end = body.find(';', start);
				if (end == std::string_view::npos) {
					end = body.size();
				}
				auto pair = body.substr(start, end - start);
				auto equals = pair.find('=');
				if (!valid(pair.substr(0, equals), true) || !valid(pair.substr(equals + 1), false)) {
					return fail("a keyword is not key=value with letters and digits");
				}
				start = end + 1;
			}
		}
	}
	result = FromName(id);
	if (result.IsBogus()) {
		return fail("it is not a locale identifier");
	}
	return true;
}

std::string Locale::GetKeyword(std::string_view key) const {
	auto at = name.find('@');
	if (at == std::string::npos || name.find('=', at) == std::string::npos) {
		return std::string();
	}
	auto keywords = std::string_view(name).substr(at + 1);
	size_t start = 0;
	while (start < keywords.size()) {
		auto end = keywords.find(';', start);
		if (end == std::string_view::npos) {
			end = keywords.size();
		}
		auto pair = keywords.substr(start, end - start);
		auto equals = pair.find('=');
		if (equals != std::string_view::npos && pair.substr(0, equals) == key) {
			return std::string(pair.substr(equals + 1));
		}
		start = end + 1;
	}
	return std::string();
}

CaseLocale Locale::GetCaseLocale() const {
	return CaseLocaleOf(GetBaseName());
}

CaseLocale Locale::CaseLocaleOf(std::string_view id) {
	auto at = [&](size_t i) {
		auto c = i < id.size() ? id[i] : '\0';
		return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
	};
	auto separator = [&](size_t i) {
		auto c = at(i);
		return c == '_' || c == '-' || c == '\0';
	};
	switch (at(0)) {
	case 'e':
		if (at(1) == 'l' && (separator(2) || (at(2) == 'l' && separator(3)))) {
			return CaseLocale::GREEK;
		}
		break;
	case 't':
		if ((at(1) == 'r' && separator(2)) || (at(1) == 'u' && at(2) == 'r' && separator(3))) {
			return CaseLocale::TURKISH;
		}
		break;
	case 'a':
		if (at(1) == 'z' && (separator(2) || (at(2) == 'e' && separator(3)))) {
			return CaseLocale::TURKISH;
		}
		break;
	case 'l':
		if ((at(1) == 't' && separator(2)) || (at(1) == 'i' && at(2) == 't' && separator(3))) {
			return CaseLocale::LITHUANIAN;
		}
		break;
	case 'n':
		if (at(1) == 'l' && (separator(2) || (at(2) == 'd' && separator(3)))) {
			return CaseLocale::DUTCH;
		}
		break;
	case 'h':
		if (at(1) == 'y' && (separator(2) || (at(2) == 'e' && separator(3)))) {
			return CaseLocale::ARMENIAN;
		}
		break;
	default:
		break;
	}
	return CaseLocale::ROOT;
}

bool Locale::GetCollation(std::string &collation) const {
	collation.clear();
	if (bogus) {
		return true;
	}
	auto keywords = std::string_view(name).substr(base_length);
	if (!keywords.empty() && keywords[0] == '@') {
		keywords.remove_prefix(1);
		while (!keywords.empty()) {
			auto end = std::min(keywords.find(';'), keywords.size());
			auto pair = keywords.substr(0, end);
			auto equals = pair.find('=');
			if (equals != std::string_view::npos) {
				auto key = pair.substr(0, equals);
				if (key.substr(0, 3) == "col" || key == "maxvariable") {
					return false;
				}
			}
			keywords.remove_prefix(std::min(end + 1, keywords.size()));
		}
	}
	std::string base(GetBaseName());
	do {
		auto end = collation_locales + collation_locale_count;
		auto entry =
		    std::lower_bound(collation_locales, end, base, [](const CollationLocale &entry, const std::string &value) {
			    return std::strcmp(entry.locale, value.c_str()) < 0;
		    });
		if (entry != end && base == entry->locale) {
			collation = entry->collation;
			return entry->supported;
		}
	} while (Truncate(base));
	return true;
}

bool Locale::Truncate(std::string &base_name) {
	auto separator = base_name.rfind('_');
	if (separator == std::string::npos) {
		if (base_name.empty()) {
			return false;
		}
		base_name.clear();
		return true;
	}
	base_name.resize(separator);
	return true;
}

} // namespace text
} // namespace duckdb
