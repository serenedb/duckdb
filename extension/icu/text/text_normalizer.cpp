#include "text_normalizer.hpp"

#include "text_data.hpp"

#include <algorithm>

namespace duckdb {
namespace text {

namespace {

constexpr uint32_t HANGUL_S_BASE = 0xAC00;
constexpr uint32_t HANGUL_L_BASE = 0x1100;
constexpr uint32_t HANGUL_V_BASE = 0x1161;
constexpr uint32_t HANGUL_T_BASE = 0x11A7;
constexpr uint32_t HANGUL_L_COUNT = 19;
constexpr uint32_t HANGUL_V_COUNT = 21;
constexpr uint32_t HANGUL_T_COUNT = 28;
constexpr uint32_t HANGUL_N_COUNT = HANGUL_V_COUNT * HANGUL_T_COUNT;
constexpr uint32_t HANGUL_S_COUNT = HANGUL_L_COUNT * HANGUL_N_COUNT;

constexpr uint32_t HAS_CANONICAL = 1;
constexpr uint32_t HAS_COMPATIBILITY = 2;
constexpr uint32_t HAS_CASEFOLD = 4;
constexpr uint32_t NFC_QC_NOT_YES = 16;
constexpr uint32_t NFKC_QC_NOT_YES = 32;

constexpr uint32_t NORMALIZATION_ARRAY_COUNT = 7;

struct NormalizationEntry {
	uint32_t canonical;
	uint32_t compatibility;
	uint32_t casefold;
	uint32_t ccc_and_flags;
};

struct NormalizationData {
	const NormalizationEntry *entries;
	const uint32_t *chars;
	const uint32_t *composition_first;
	const uint32_t *composition_second;
	const uint32_t *composition_result;
	uint32_t composition_count;
	uint32_t composition_min_second;
	CodePointTable table;
	uint8_t two_byte_passes[0x800];
	uint64_t bmp_nonspacing_marks[0x10000 / 64];

	const NormalizationEntry &Get(uint32_t c) const {
		return entries[table.Get(c)];
	}
};

bool IsHangulSyllable(uint32_t c) {
	return c >= HANGUL_S_BASE && c < HANGUL_S_BASE + HANGUL_S_COUNT;
}

bool IsComposing(NormalizationForm form) {
	return form == NormalizationForm::NFC || form == NormalizationForm::NFKC || form == NormalizationForm::NFKC_CF;
}

bool EntryPassesThrough(const NormalizationData &data, NormalizationForm form, uint32_t c) {
	if (c < 0x80 || c > 0x10FFFF) {
		return true;
	}
	if (IsHangulSyllable(c)) {
		return IsComposing(form);
	}
	auto ccc_and_flags = data.Get(c).ccc_and_flags;
	if ((ccc_and_flags & 0xFF) != 0) {
		return false;
	}
	auto flags = ccc_and_flags >> 8;
	switch (form) {
	case NormalizationForm::NFC:
		return (flags & NFC_QC_NOT_YES) == 0;
	case NormalizationForm::NFKC:
		return (flags & NFKC_QC_NOT_YES) == 0;
	case NormalizationForm::NFKC_CF:
		return (flags & (NFKC_QC_NOT_YES | HAS_CASEFOLD)) == 0;
	case NormalizationForm::NFD:
		return (flags & HAS_CANONICAL) == 0;
	case NormalizationForm::NFKD:
		return (flags & HAS_COMPATIBILITY) == 0;
	}
	return false;
}

const NormalizationData &GetData() {
	static const NormalizationData data = []() {
		TextUnitReader reader(LoadUnit(text_units[text_normalization_unit]), NORMALIZATION_ARRAY_COUNT);
		NormalizationData result {};
		result.entries = reinterpret_cast<const NormalizationEntry *>(reader.Read<uint32_t>());
		result.chars = reader.Read<uint32_t>();
		result.composition_first = reader.Read<uint32_t>(result.composition_count);
		result.composition_second = reader.Read<uint32_t>();
		result.composition_result = reader.Read<uint32_t>();
		result.table.stage1 = reader.Read<uint16_t>();
		result.table.stage2 = reader.Read<uint16_t>();
		result.composition_min_second = std::min(HANGUL_V_BASE, HANGUL_T_BASE + 1);
		for (uint32_t i = 0; i < result.composition_count; i++) {
			result.composition_min_second = std::min(result.composition_min_second, result.composition_second[i]);
		}
		for (uint32_t c = 0; c < 0x800; c++) {
			for (auto form : {NormalizationForm::NFC, NormalizationForm::NFD, NormalizationForm::NFKC,
			                  NormalizationForm::NFKD, NormalizationForm::NFKC_CF}) {
				if (EntryPassesThrough(result, form, c)) {
					result.two_byte_passes[c] |= static_cast<uint8_t>(1 << static_cast<uint8_t>(form));
				}
			}
		}
		for (uint32_t i = 0; i < nonspacing_marks_count; i++) {
			auto last = std::min<uint32_t>(nonspacing_marks[i].last, 0xFFFF);
			for (auto c = nonspacing_marks[i].first; c <= last; c++) {
				result.bmp_nonspacing_marks[c / 64] |= uint64_t(1) << (c % 64);
			}
		}
		return result;
	}();
	return data;
}

void AppendMapping(const NormalizationData &data, uint32_t mapping, std::vector<uint32_t> &output) {
	auto offset = mapping & 0xFFFFFF;
	auto length = mapping >> 24;
	if (length == 1) {
		output.push_back(data.chars[offset]);
		return;
	}
	output.insert(output.end(), data.chars + offset, data.chars + offset + length);
}

bool PassesThrough(const NormalizationData &data, NormalizationForm form, uint32_t c) {
	if (c < 0x800) {
		return (data.two_byte_passes[c] >> static_cast<uint8_t>(form)) & 1;
	}
	return EntryPassesThrough(data, form, c);
}

uint32_t PassThroughValue(NormalizationForm form, uint32_t c) {
	return form == NormalizationForm::NFKC_CF && c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

void Decompose(const NormalizationData &data, NormalizationForm form, const uint32_t *input, size_t size,
               std::vector<uint32_t> &output) {
	for (size_t i = 0; i < size; i++) {
		auto c = input[i];
		if (c < 0x80) {
			output.push_back(form == NormalizationForm::NFKC_CF && c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
			continue;
		}
		if (IsHangulSyllable(c)) {
			auto index = c - HANGUL_S_BASE;
			output.push_back(HANGUL_L_BASE + index / HANGUL_N_COUNT);
			output.push_back(HANGUL_V_BASE + (index % HANGUL_N_COUNT) / HANGUL_T_COUNT);
			if (index % HANGUL_T_COUNT != 0) {
				output.push_back(HANGUL_T_BASE + index % HANGUL_T_COUNT);
			}
			continue;
		}
		if (c > 0x10FFFF) {
			output.push_back(c);
			continue;
		}
		auto &entry = data.Get(c);
		auto flags = entry.ccc_and_flags >> 8;
		switch (form) {
		case NormalizationForm::NFC:
		case NormalizationForm::NFD:
			if (flags & HAS_CANONICAL) {
				AppendMapping(data, entry.canonical, output);
				continue;
			}
			break;
		case NormalizationForm::NFKC:
		case NormalizationForm::NFKD:
			if (flags & HAS_COMPATIBILITY) {
				AppendMapping(data, entry.compatibility, output);
				continue;
			}
			break;
		case NormalizationForm::NFKC_CF:
			if (flags & HAS_CASEFOLD) {
				AppendMapping(data, entry.casefold, output);
				continue;
			}
			if (flags & HAS_CANONICAL) {
				AppendMapping(data, entry.canonical, output);
				continue;
			}
			break;
		}
		output.push_back(c);
	}
}

uint8_t GetCombiningClass(const NormalizationData &data, uint32_t c) {
	if (c < 0x300 || c > 0x10FFFF) {
		return 0;
	}
	return static_cast<uint8_t>(data.Get(c).ccc_and_flags & 0xFF);
}

void CanonicalOrder(const NormalizationData &data, std::vector<uint32_t> &text, size_t begin) {
	for (size_t i = begin + 1; i < text.size(); i++) {
		auto current = GetCombiningClass(data, text[i]);
		if (current == 0) {
			continue;
		}
		for (size_t j = i; j > begin; j--) {
			auto previous = GetCombiningClass(data, text[j - 1]);
			if (previous == 0 || previous <= current) {
				break;
			}
			std::swap(text[j - 1], text[j]);
		}
	}
}

uint32_t ComposePair(const NormalizationData &data, uint32_t first, uint32_t second) {
	if (first >= HANGUL_L_BASE && first < HANGUL_L_BASE + HANGUL_L_COUNT && second >= HANGUL_V_BASE &&
	    second < HANGUL_V_BASE + HANGUL_V_COUNT) {
		return HANGUL_S_BASE + ((first - HANGUL_L_BASE) * HANGUL_V_COUNT + (second - HANGUL_V_BASE)) * HANGUL_T_COUNT;
	}
	if (IsHangulSyllable(first) && (first - HANGUL_S_BASE) % HANGUL_T_COUNT == 0 && second > HANGUL_T_BASE &&
	    second < HANGUL_T_BASE + HANGUL_T_COUNT) {
		return first + (second - HANGUL_T_BASE);
	}
	uint32_t lower = 0;
	uint32_t upper = data.composition_count;
	while (lower < upper) {
		auto middle = (lower + upper) / 2;
		auto f = data.composition_first[middle];
		auto s = data.composition_second[middle];
		if (first < f || (first == f && second < s)) {
			upper = middle;
		} else if (first > f || second > s) {
			lower = middle + 1;
		} else {
			return data.composition_result[middle];
		}
	}
	return 0;
}

void Compose(const NormalizationData &data, std::vector<uint32_t> &text, size_t begin) {
	if (begin == text.size()) {
		return;
	}
	size_t starter_position = begin;
	uint32_t starter = text[begin];
	uint32_t last_class = GetCombiningClass(data, starter);
	if (last_class != 0) {
		last_class = 256;
	}
	size_t composed_position = begin + 1;
	for (size_t i = begin + 1; i < text.size(); i++) {
		auto c = text[i];
		uint32_t c_class = GetCombiningClass(data, c);
		auto composite = c >= data.composition_min_second ? ComposePair(data, starter, c) : 0;
		if (composite != 0 && (last_class < c_class || last_class == 0)) {
			text[starter_position] = composite;
			starter = composite;
			continue;
		}
		if (c_class == 0) {
			starter_position = composed_position;
			starter = c;
		}
		last_class = c_class;
		text[composed_position++] = c;
	}
	text.resize(composed_position);
}

void AppendSegment(const NormalizationData &data, NormalizationForm form, const uint32_t *input, size_t size,
                   std::vector<uint32_t> &output) {
	auto begin = output.size();
	Decompose(data, form, input, size, output);
	CanonicalOrder(data, output, begin);
	if (IsComposing(form)) {
		Compose(data, output, begin);
	}
}

} // namespace

void Normalizer::Normalize(NormalizationForm form, const uint32_t *input, size_t size, std::vector<uint32_t> &output) {
	auto &data = GetData();
	output.clear();
	output.reserve(size);
	size_t starter = size;
	size_t position = 0;
	while (position < size) {
		auto c = input[position];
		if (PassesThrough(data, form, c)) {
			starter = position++;
			output.push_back(PassThroughValue(form, c));
			continue;
		}
		size_t begin = position;
		if (starter == position - 1) {
			begin = starter;
			output.pop_back();
		}
		size_t end = position + 1;
		while (end < size && !PassesThrough(data, form, input[end])) {
			end++;
		}
		AppendSegment(data, form, input + begin, end - begin, output);
		position = end;
	}
}

bool Normalizer::IsNormalized(NormalizationForm form, const uint32_t *input, size_t size,
                              std::vector<uint32_t> &scratch) {
	bool ascii = form != NormalizationForm::NFKC_CF;
	for (size_t i = 0; ascii && i < size; i++) {
		ascii = input[i] < 0x80;
	}
	if (ascii) {
		return true;
	}
	Normalize(form, input, size, scratch);
	return scratch.size() == size && std::equal(scratch.begin(), scratch.end(), input);
}

bool Normalizer::IsInert(NormalizationForm form, uint32_t c) {
	return PassesThrough(GetData(), form, c);
}

bool Normalizer::IsNonspacingMark(uint32_t c) {
	if (c < 0x10000) {
		return (GetData().bmp_nonspacing_marks[c / 64] >> (c % 64)) & 1;
	}
	return RangesContain(nonspacing_marks, nonspacing_marks_count, c);
}

void Normalizer::RemoveNonspacingMarks(std::vector<uint32_t> &text) {
	text.erase(std::remove_if(text.begin(), text.end(), [](uint32_t c) { return IsNonspacingMark(c); }), text.end());
}

bool Normalizer::HasNfkcBoundaryBefore(uint32_t c) {
	return !RangesContain(nfkc_no_boundary_before, nfkc_no_boundary_before_count, c);
}

uint8_t Normalizer::CombiningClass(uint32_t c) {
	return GetCombiningClass(GetData(), c);
}

} // namespace text
} // namespace duckdb
