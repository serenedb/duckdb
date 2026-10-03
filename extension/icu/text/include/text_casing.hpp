//===----------------------------------------------------------------------===//
//                         DuckDB
//
// text_casing.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "text_locale.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace duckdb {
namespace text {

enum class CaseFolding : uint8_t { DEFAULT, TURKIC };

enum class CaseMapping : uint8_t { NONE, LOWER, UPPER, FOLD, SIMPLE_LOWER, SIMPLE_UPPER };

class CaseMap {
public:
	static void ToLower(CaseLocale locale, const uint32_t *input, size_t size, std::vector<uint32_t> &output);
	static void ToLower(CaseLocale locale, const uint32_t *text, size_t size, size_t begin, size_t end,
	                    std::vector<uint32_t> &output);
	static void ToUpper(CaseLocale locale, const uint32_t *input, size_t size, std::vector<uint32_t> &output);
	static void Fold(CaseFolding folding, const uint32_t *input, size_t size, std::vector<uint32_t> &output);

	static uint32_t SimpleLower(uint32_t c);
	static uint32_t SimpleUpper(uint32_t c);

	static bool IsCased(uint32_t c);
	static bool IsCaseIgnorable(uint32_t c);
	static bool IsInvariant(CaseMapping mapping, CaseLocale locale, CaseFolding folding, uint32_t c);
};

} // namespace text
} // namespace duckdb
