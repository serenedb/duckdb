//===----------------------------------------------------------------------===//
//                         DuckDB
//
// text_normalizer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace duckdb {
namespace text {

enum class NormalizationForm : uint8_t { NFC, NFD, NFKC, NFKD, NFKC_CF };

class Normalizer {
public:
	static void Normalize(NormalizationForm form, const uint32_t *input, size_t size, std::vector<uint32_t> &output);
	static bool IsNormalized(NormalizationForm form, const uint32_t *input, size_t size,
	                         std::vector<uint32_t> &scratch);
	static bool IsInert(NormalizationForm form, uint32_t c);

	static void RemoveNonspacingMarks(std::vector<uint32_t> &text);
	static bool IsNonspacingMark(uint32_t c);

	static bool HasNfkcBoundaryBefore(uint32_t c);
	static uint8_t CombiningClass(uint32_t c);
};

} // namespace text
} // namespace duckdb
