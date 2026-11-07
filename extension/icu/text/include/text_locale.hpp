//===----------------------------------------------------------------------===//
//                         DuckDB
//
// text_locale.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace duckdb {
namespace text {

enum class CaseLocale : uint8_t { ROOT, TURKISH, LITHUANIAN, GREEK, DUTCH, ARMENIAN };

class Locale {
public:
	Locale() = default;

	static Locale FromName(std::string_view name);
	static bool TryParse(std::string_view name, Locale &result, std::string &error);

	bool IsBogus() const {
		return bogus;
	}
	const std::string &GetName() const {
		return name;
	}
	std::string_view GetLanguage() const {
		return std::string_view(name).substr(0, language_length);
	}
	std::string_view GetScript() const {
		return std::string_view(name).substr(script_offset, script_length);
	}
	std::string_view GetRegion() const {
		return std::string_view(name).substr(region_offset, region_length);
	}
	std::string_view GetVariant() const {
		return std::string_view(name).substr(variant_offset, variant_length);
	}
	std::string_view GetBaseName() const {
		return std::string_view(name).substr(0, base_length);
	}
	std::string GetKeyword(std::string_view key) const;

	CaseLocale GetCaseLocale() const;
	static CaseLocale CaseLocaleOf(std::string_view id);
	bool GetCollation(std::string &collation) const;

	static bool Truncate(std::string &base_name);

	bool operator==(const Locale &other) const {
		return bogus == other.bogus && name == other.name;
	}

private:
	std::string name;
	bool bogus = true;
	uint16_t language_length = 0;
	uint16_t script_offset = 0;
	uint16_t script_length = 0;
	uint16_t region_offset = 0;
	uint16_t region_length = 0;
	uint16_t variant_offset = 0;
	uint16_t variant_length = 0;
	uint16_t base_length = 0;
};

} // namespace text
} // namespace duckdb
