#include "text_casing.hpp"

#include "text_data.hpp"

#include <initializer_list>

namespace duckdb {
namespace text {

namespace {

constexpr uint32_t CASE_ARRAY_COUNT = 4;

constexpr uint32_t CASE_CASED = 1;
constexpr uint32_t CASE_IGNORABLE = 2;
constexpr uint32_t CASE_DOT_SHIFT = 2;

enum class DotType : uint8_t { NO_DOT = 0, SOFT_DOTTED = 1, ABOVE = 2, OTHER_ACCENT = 3 };

constexpr uint32_t FULL_MAPPING = 1u << 31;

struct CaseEntry {
	uint32_t lower_delta;
	uint32_t upper_delta;
	uint32_t full_lower;
	uint32_t full_upper;
	uint32_t full_fold;
	uint32_t flags;
};

struct CaseData {
	const CaseEntry *entries;
	const uint32_t *chars;
	CodePointTable table;

	const CaseEntry &Get(uint32_t c) const {
		return entries[table.Get(c)];
	}

	DotType GetDotType(uint32_t c) const {
		return static_cast<DotType>((Get(c).flags >> CASE_DOT_SHIFT) & 3);
	}

	void Append(uint32_t c, uint32_t full, uint32_t delta, std::vector<uint32_t> &output) const {
		if (full & FULL_MAPPING) {
			auto offset = full & 0xFFFFFF;
			auto length = (full >> 24) & 0x7F;
			output.insert(output.end(), chars + offset, chars + offset + length);
		} else {
			output.push_back(c + delta);
		}
	}
};

const CaseData &GetData() {
	static const CaseData data = []() {
		TextUnitReader reader(LoadUnit(text_units[text_case_unit]), CASE_ARRAY_COUNT);
		CaseData result {};
		result.entries = reinterpret_cast<const CaseEntry *>(reader.Read<uint32_t>());
		result.chars = reader.Read<uint32_t>();
		result.table.stage1 = reader.Read<uint16_t>();
		result.table.stage2 = reader.Read<uint16_t>();
		return result;
	}();
	return data;
}

bool CasedLetterFollows(const CaseData &data, const uint32_t *text, size_t size, size_t position) {
	for (; position < size; position++) {
		auto flags = data.Get(text[position]).flags;
		if (!(flags & CASE_IGNORABLE)) {
			return (flags & CASE_CASED) != 0;
		}
	}
	return false;
}

bool CasedLetterPrecedes(const CaseData &data, const uint32_t *text, size_t position) {
	while (position > 0) {
		auto flags = data.Get(text[--position]).flags;
		if (!(flags & CASE_IGNORABLE)) {
			return (flags & CASE_CASED) != 0;
		}
	}
	return false;
}

bool IsPrecededBySoftDotted(const CaseData &data, const uint32_t *text, size_t position) {
	while (position > 0) {
		auto dot = data.GetDotType(text[--position]);
		if (dot == DotType::SOFT_DOTTED) {
			return true;
		}
		if (dot != DotType::OTHER_ACCENT) {
			return false;
		}
	}
	return false;
}

bool IsPrecededByCapitalI(const CaseData &data, const uint32_t *text, size_t position) {
	while (position > 0) {
		auto c = text[--position];
		if (c == 0x49) {
			return true;
		}
		if (data.GetDotType(c) != DotType::OTHER_ACCENT) {
			return false;
		}
	}
	return false;
}

bool IsFollowedByMoreAbove(const CaseData &data, const uint32_t *text, size_t size, size_t position) {
	for (position++; position < size; position++) {
		auto dot = data.GetDotType(text[position]);
		if (dot == DotType::ABOVE) {
			return true;
		}
		if (dot != DotType::OTHER_ACCENT) {
			return false;
		}
	}
	return false;
}

bool IsFollowedByDotAbove(const CaseData &data, const uint32_t *text, size_t size, size_t position) {
	for (position++; position < size; position++) {
		auto c = text[position];
		if (c == 0x307) {
			return true;
		}
		if (data.GetDotType(c) != DotType::OTHER_ACCENT) {
			return false;
		}
	}
	return false;
}

void Append(std::vector<uint32_t> &output, std::initializer_list<uint32_t> chars) {
	output.insert(output.end(), chars);
}

void LowerAt(const CaseData &data, CaseLocale locale, const uint32_t *text, size_t size, size_t position,
             std::vector<uint32_t> &output) {
	auto c = text[position];
	switch (c) {
	case 0x49:
		if (locale == CaseLocale::LITHUANIAN && IsFollowedByMoreAbove(data, text, size, position)) {
			return Append(output, {0x69, 0x307});
		}
		if (locale == CaseLocale::TURKISH && !IsFollowedByDotAbove(data, text, size, position)) {
			return Append(output, {0x131});
		}
		break;
	case 0x4A:
		if (locale == CaseLocale::LITHUANIAN && IsFollowedByMoreAbove(data, text, size, position)) {
			return Append(output, {0x6A, 0x307});
		}
		break;
	case 0x12E:
		if (locale == CaseLocale::LITHUANIAN && IsFollowedByMoreAbove(data, text, size, position)) {
			return Append(output, {0x12F, 0x307});
		}
		break;
	case 0xCC:
		if (locale == CaseLocale::LITHUANIAN) {
			return Append(output, {0x69, 0x307, 0x300});
		}
		break;
	case 0xCD:
		if (locale == CaseLocale::LITHUANIAN) {
			return Append(output, {0x69, 0x307, 0x301});
		}
		break;
	case 0x128:
		if (locale == CaseLocale::LITHUANIAN) {
			return Append(output, {0x69, 0x307, 0x303});
		}
		break;
	case 0x130:
		if (locale == CaseLocale::TURKISH) {
			return Append(output, {0x69});
		}
		break;
	case 0x307:
		if (locale == CaseLocale::TURKISH && IsPrecededByCapitalI(data, text, position)) {
			return;
		}
		break;
	case 0x3A3:
		if (!CasedLetterFollows(data, text, size, position + 1) && CasedLetterPrecedes(data, text, position)) {
			return Append(output, {0x3C2});
		}
		break;
	default:
		break;
	}
	auto &entry = data.Get(c);
	data.Append(c, entry.full_lower, entry.lower_delta, output);
}

void UpperAt(const CaseData &data, CaseLocale locale, const uint32_t *text, size_t position,
             std::vector<uint32_t> &output) {
	auto c = text[position];
	switch (c) {
	case 0x69:
		if (locale == CaseLocale::TURKISH) {
			return Append(output, {0x130});
		}
		break;
	case 0x307:
		if (locale == CaseLocale::LITHUANIAN && IsPrecededBySoftDotted(data, text, position)) {
			return;
		}
		break;
	case 0x587:
		if (locale == CaseLocale::ARMENIAN) {
			return Append(output, {0x535, 0x54E});
		}
		break;
	default:
		break;
	}
	auto &entry = data.Get(c);
	data.Append(c, entry.full_upper, entry.upper_delta, output);
}

constexpr uint32_t GREEK_UPPER_MASK = 0x3FF;
constexpr uint32_t GREEK_HAS_VOWEL = 0x1000;
constexpr uint32_t GREEK_HAS_YPOGEGRAMMENI = 0x2000;
constexpr uint32_t GREEK_HAS_ACCENT = 0x4000;
constexpr uint32_t GREEK_HAS_DIALYTIKA = 0x8000;
constexpr uint32_t GREEK_HAS_COMBINING_DIALYTIKA = 0x10000;
constexpr uint32_t GREEK_HAS_OTHER_DIACRITIC = 0x20000;
constexpr uint32_t GREEK_HAS_VOWEL_AND_ACCENT = GREEK_HAS_VOWEL | GREEK_HAS_ACCENT;
constexpr uint32_t GREEK_HAS_VOWEL_AND_ACCENT_AND_DIALYTIKA = GREEK_HAS_VOWEL_AND_ACCENT | GREEK_HAS_DIALYTIKA;
constexpr uint32_t GREEK_HAS_EITHER_DIALYTIKA = GREEK_HAS_DIALYTIKA | GREEK_HAS_COMBINING_DIALYTIKA;

constexpr uint32_t GREEK_AFTER_CASED = 1;
constexpr uint32_t GREEK_AFTER_VOWEL_WITH_COMBINING_ACCENT = 2;
constexpr uint32_t GREEK_AFTER_VOWEL_WITH_PRECOMPOSED_ACCENT = 4;

uint32_t GreekLetterData(uint32_t c) {
	if (c < 0x370 || c > 0x2126 || (c > 0x3FF && c < 0x1F00)) {
		return 0;
	}
	if (c <= 0x3FF) {
		return greek_upper_0370[c - 0x370];
	}
	if (c <= 0x1FFF) {
		return greek_upper_1f00[c - 0x1F00];
	}
	return c == 0x2126 ? greek_upper_2126 : 0;
}

uint32_t GreekDiacriticData(uint32_t c) {
	switch (c) {
	case 0x300:
	case 0x301:
	case 0x342:
	case 0x302:
	case 0x303:
	case 0x311:
		return GREEK_HAS_ACCENT;
	case 0x308:
		return GREEK_HAS_COMBINING_DIALYTIKA;
	case 0x344:
		return GREEK_HAS_COMBINING_DIALYTIKA | GREEK_HAS_ACCENT;
	case 0x345:
		return GREEK_HAS_YPOGEGRAMMENI;
	case 0x304:
	case 0x306:
	case 0x313:
	case 0x314:
	case 0x343:
		return GREEK_HAS_OTHER_DIACRITIC;
	default:
		return 0;
	}
}

void GreekToUpper(const CaseData &data, const uint32_t *input, size_t size, std::vector<uint32_t> &output) {
	uint32_t state = 0;
	for (size_t i = 0; i < size;) {
		auto c = input[i];
		size_t next = i + 1;
		uint32_t next_state = 0;
		auto flags = data.Get(c).flags;
		if (flags & CASE_IGNORABLE) {
			next_state |= state & GREEK_AFTER_CASED;
		} else if (flags & CASE_CASED) {
			next_state |= GREEK_AFTER_CASED;
		}
		auto letter = GreekLetterData(c);
		if (letter == 0) {
			auto &entry = data.Get(c);
			data.Append(c, entry.full_upper, entry.upper_delta, output);
			i = next;
			state = next_state;
			continue;
		}
		auto upper = letter & GREEK_UPPER_MASK;
		if ((letter & GREEK_HAS_VOWEL) &&
		    (state & (GREEK_AFTER_VOWEL_WITH_PRECOMPOSED_ACCENT | GREEK_AFTER_VOWEL_WITH_COMBINING_ACCENT)) &&
		    (upper == 0x399 || upper == 0x3A5)) {
			letter |= (state & GREEK_AFTER_VOWEL_WITH_PRECOMPOSED_ACCENT) ? GREEK_HAS_DIALYTIKA
			                                                              : GREEK_HAS_COMBINING_DIALYTIKA;
		}
		uint32_t ypogegrammeni = (letter & GREEK_HAS_YPOGEGRAMMENI) ? 1 : 0;
		bool precomposed_accent = (letter & GREEK_HAS_ACCENT) != 0;
		while (next < size) {
			auto diacritic = GreekDiacriticData(input[next]);
			if (diacritic == 0) {
				break;
			}
			letter |= diacritic;
			if (diacritic & GREEK_HAS_YPOGEGRAMMENI) {
				ypogegrammeni++;
			}
			next++;
		}
		if ((letter & GREEK_HAS_VOWEL_AND_ACCENT_AND_DIALYTIKA) == GREEK_HAS_VOWEL_AND_ACCENT) {
			next_state |= precomposed_accent ? GREEK_AFTER_VOWEL_WITH_PRECOMPOSED_ACCENT
			                                 : GREEK_AFTER_VOWEL_WITH_COMBINING_ACCENT;
		}
		bool add_tonos = false;
		if (upper == 0x397 && (letter & GREEK_HAS_ACCENT) && ypogegrammeni == 0 && !(state & GREEK_AFTER_CASED) &&
		    !CasedLetterFollows(data, input, size, next)) {
			if (precomposed_accent) {
				upper = 0x389;
			} else {
				add_tonos = true;
			}
		} else if (letter & GREEK_HAS_DIALYTIKA) {
			if (upper == 0x399) {
				upper = 0x3AA;
				letter &= ~GREEK_HAS_EITHER_DIALYTIKA;
			} else if (upper == 0x3A5) {
				upper = 0x3AB;
				letter &= ~GREEK_HAS_EITHER_DIALYTIKA;
			}
		}
		output.push_back(upper);
		if (letter & GREEK_HAS_EITHER_DIALYTIKA) {
			output.push_back(0x308);
		}
		if (add_tonos) {
			output.push_back(0x301);
		}
		for (; ypogegrammeni > 0; ypogegrammeni--) {
			output.push_back(0x399);
		}
		i = next;
		state = next_state;
	}
}

} // namespace

void CaseMap::ToLower(CaseLocale locale, const uint32_t *input, size_t size, std::vector<uint32_t> &output) {
	ToLower(locale, input, size, 0, size, output);
}

void CaseMap::ToLower(CaseLocale locale, const uint32_t *text, size_t size, size_t begin, size_t end,
                      std::vector<uint32_t> &output) {
	auto &data = GetData();
	output.clear();
	output.reserve(end - begin);
	bool ascii = locale != CaseLocale::TURKISH && locale != CaseLocale::LITHUANIAN;
	for (size_t i = begin; i < end; i++) {
		auto c = text[i];
		if (c < 0x80 && (ascii || (c != 0x49 && c != 0x4A))) {
			output.push_back(c >= 'A' && c <= 'Z' ? c + 0x20 : c);
			continue;
		}
		LowerAt(data, locale, text, size, i, output);
	}
}

void CaseMap::ToUpper(CaseLocale locale, const uint32_t *input, size_t size, std::vector<uint32_t> &output) {
	auto &data = GetData();
	output.clear();
	output.reserve(size);
	if (locale == CaseLocale::GREEK) {
		GreekToUpper(data, input, size, output);
		return;
	}
	bool ascii = locale != CaseLocale::TURKISH;
	for (size_t i = 0; i < size; i++) {
		auto c = input[i];
		if (c < 0x80 && (ascii || c != 0x69)) {
			output.push_back(c >= 'a' && c <= 'z' ? c - 0x20 : c);
			continue;
		}
		UpperAt(data, locale, input, i, output);
	}
}

void CaseMap::Fold(CaseFolding folding, const uint32_t *input, size_t size, std::vector<uint32_t> &output) {
	auto &data = GetData();
	output.clear();
	output.reserve(size);
	for (size_t i = 0; i < size; i++) {
		auto c = input[i];
		if (folding == CaseFolding::TURKIC) {
			if (c == 0x49) {
				output.push_back(0x131);
				continue;
			}
			if (c == 0x130) {
				output.push_back(0x69);
				continue;
			}
		}
		if (c < 0x80) {
			output.push_back(c >= 'A' && c <= 'Z' ? c + 0x20 : c);
			continue;
		}
		data.Append(c, data.Get(c).full_fold, 0, output);
	}
}

uint32_t CaseMap::SimpleLower(uint32_t c) {
	if (c < 0x80) {
		return c >= 'A' && c <= 'Z' ? c + 0x20 : c;
	}
	return c > 0x10FFFF ? c : c + GetData().Get(c).lower_delta;
}

uint32_t CaseMap::SimpleUpper(uint32_t c) {
	if (c < 0x80) {
		return c >= 'a' && c <= 'z' ? c - 0x20 : c;
	}
	return c > 0x10FFFF ? c : c + GetData().Get(c).upper_delta;
}

bool CaseMap::IsCased(uint32_t c) {
	return (GetData().Get(c).flags & CASE_CASED) != 0;
}

bool CaseMap::IsCaseIgnorable(uint32_t c) {
	return (GetData().Get(c).flags & CASE_IGNORABLE) != 0;
}

bool CaseMap::IsInvariant(CaseMapping mapping, CaseLocale locale, CaseFolding folding, uint32_t c) {
	if (c < 0x80) {
		switch (mapping) {
		case CaseMapping::NONE:
			return true;
		case CaseMapping::UPPER:
		case CaseMapping::SIMPLE_UPPER:
			return c < 'a' || c > 'z';
		case CaseMapping::LOWER:
		case CaseMapping::FOLD:
		case CaseMapping::SIMPLE_LOWER:
			return c < 'A' || c > 'Z';
		}
	}
	auto &entry = GetData().Get(c);
	switch (mapping) {
	case CaseMapping::NONE:
		return true;
	case CaseMapping::LOWER:
		if (c == 0x307 && locale == CaseLocale::TURKISH) {
			return false;
		}
		return (entry.full_lower & FULL_MAPPING) == 0 && entry.lower_delta == 0;
	case CaseMapping::UPPER:
		if (c == 0x307 && locale == CaseLocale::LITHUANIAN) {
			return false;
		}
		if (locale == CaseLocale::GREEK && (GreekLetterData(c) != 0 || GreekDiacriticData(c) != 0)) {
			return false;
		}
		return (entry.full_upper & FULL_MAPPING) == 0 && entry.upper_delta == 0;
	case CaseMapping::FOLD:
		if (folding == CaseFolding::TURKIC && c == 0x130) {
			return false;
		}
		return (entry.full_fold & FULL_MAPPING) == 0;
	case CaseMapping::SIMPLE_LOWER:
		return entry.lower_delta == 0;
	case CaseMapping::SIMPLE_UPPER:
		return entry.upper_delta == 0;
	}
	return false;
}

} // namespace text
} // namespace duckdb
