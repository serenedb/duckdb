#include "text_transform.hpp"

#include "text_utf8.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>

namespace duckdb {
namespace text {

namespace {

constexpr uint32_t UNSTABLE = 0xFFFFFFFF;
constexpr uint32_t NO_CONTEXT = 0xFFFFFFFF;
constexpr uint32_t CAPITAL_SIGMA = 0x3A3;

constexpr size_t FORM_COUNT = 5;
constexpr size_t CASE_MAPPING_COUNT = 6;
constexpr size_t CASE_LOCALE_COUNT = 6;
constexpr size_t CASE_FOLDING_COUNT = 2;
constexpr size_t TABLES_COUNT = FORM_COUNT * CASE_MAPPING_COUNT * CASE_LOCALE_COUNT * CASE_FOLDING_COUNT * 2 * 2;

bool IsComposing(NormalizationForm form) {
	return form == NormalizationForm::NFC || form == NormalizationForm::NFKC || form == NormalizationForm::NFKC_CF;
}

NormalizationForm BoundaryForm(NormalizationForm form) {
	return form == NormalizationForm::NFKC_CF ? NormalizationForm::NFKC : form;
}

TransformOptions Canonical(TransformOptions options) {
	if (options.case_mapping != CaseMapping::FOLD) {
		options.folding = CaseFolding::DEFAULT;
	}
	switch (options.case_mapping) {
	case CaseMapping::LOWER:
		if (options.locale != CaseLocale::TURKISH && options.locale != CaseLocale::LITHUANIAN) {
			options.locale = CaseLocale::ROOT;
		}
		break;
	case CaseMapping::UPPER:
		if (options.locale == CaseLocale::DUTCH) {
			options.locale = CaseLocale::ROOT;
		}
		break;
	case CaseMapping::NONE:
	case CaseMapping::FOLD:
	case CaseMapping::SIMPLE_LOWER:
	case CaseMapping::SIMPLE_UPPER:
		options.locale = CaseLocale::ROOT;
		break;
	}
	if (!options.strip_marks || options.case_mapping == CaseMapping::NONE) {
		options.strip_before_case = false;
	}
	return options;
}

size_t TablesIndex(const TransformOptions &options) {
	auto index = static_cast<size_t>(options.form);
	index = index * CASE_MAPPING_COUNT + static_cast<size_t>(options.case_mapping);
	index = index * CASE_LOCALE_COUNT + static_cast<size_t>(options.locale);
	index = index * CASE_FOLDING_COUNT + static_cast<size_t>(options.folding);
	index = index * 2 + (options.strip_marks ? 1 : 0);
	return index * 2 + (options.strip_before_case ? 1 : 0);
}

uint64_t UpperMask(uint64_t word) {
	return (word + 0x3F3F3F3F3F3F3F3FULL) & ~(word + 0x2525252525252525ULL) & 0x8080808080808080ULL;
}

uint64_t LowerMask(uint64_t word) {
	return (word + 0x1F1F1F1F1F1F1F1FULL) & ~(word + 0x0505050505050505ULL) & 0x8080808080808080ULL;
}

void Decode(const uint8_t *bytes, size_t begin, size_t end, std::vector<uint32_t> &output) {
	output.clear();
	while (begin < end) {
		output.push_back(DecodeUtf8(bytes, end, begin));
	}
}

void NormalizeInPlace(NormalizationForm form, TransformBuffer &buffer) {
	Normalizer::Normalize(form, buffer.text.data(), buffer.text.size(), buffer.scratch);
	buffer.text.swap(buffer.scratch);
}

bool ContainsCapitalSigma(const std::vector<uint32_t> &text) {
	return std::find(text.begin(), text.end(), CAPITAL_SIGMA) != text.end();
}

uint32_t PrecedingContext(const uint8_t *text, size_t end) {
	while (end > 0) {
		auto begin = end - 1;
		while (begin > 0 && (text[begin] & 0xC0) == 0x80) {
			begin--;
		}
		auto cursor = begin;
		auto c = DecodeUtf8(text, end, cursor);
		if (!CaseMap::IsCaseIgnorable(c)) {
			return c;
		}
		end = begin;
	}
	return NO_CONTEXT;
}

uint16_t TableEntry(uint32_t mapped) {
	return mapped > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(mapped);
}

} // namespace

Transform::Transform(const TransformOptions &options_p) : options(Canonical(options_p)) {
	renormalize_form = BoundaryForm(options.form);
	strip_form = options.form == NormalizationForm::NFC || options.form == NormalizationForm::NFD
	                 ? NormalizationForm::NFD
	                 : NormalizationForm::NFKD;
	composes = IsComposing(options.form);
	sigma_context = options.case_mapping == CaseMapping::LOWER && options.form != NormalizationForm::NFKC_CF;
	whole_value = options.case_mapping == CaseMapping::UPPER && options.locale == CaseLocale::GREEK;
	tables = &GetTables();

	bool identity = true;
	bool lower = true;
	bool upper = true;
	for (uint32_t c = 0; c < 0x80; c++) {
		auto mapped = tables->ascii[c];
		auto is_upper = c >= 'A' && c <= 'Z';
		auto is_lower = c >= 'a' && c <= 'z';
		identity = identity && mapped == c;
		lower = lower && mapped == (is_upper ? c + ('a' - 'A') : c);
		upper = upper && mapped == (is_lower ? c - ('a' - 'A') : c);
	}
	ascii_mode = identity ? AsciiMode::IDENTITY
	             : lower  ? AsciiMode::LOWER
	             : upper  ? AsciiMode::UPPER
	                      : AsciiMode::TABLE;
}

const Transform::Tables &Transform::GetTables() const {
	static std::atomic<const Tables *> cache[TABLES_COUNT];
	auto &slot = cache[TablesIndex(options)];
	if (auto cached = slot.load(std::memory_order_acquire)) {
		return *cached;
	}
	auto built = std::make_unique<Tables>();
	TransformBuffer buffer;
	for (uint32_t c = 0; c < 0x80; c++) {
		built->ascii[c] = TableEntry(StableMapping(c, buffer));
		built->two_byte[c] = Tables::UNSTABLE;
	}
	for (uint32_t c = 0x80; c < 0x800; c++) {
		built->two_byte[c] = TableEntry(StableMapping(c, buffer));
	}
	const Tables *expected = nullptr;
	if (slot.compare_exchange_strong(expected, built.get(), std::memory_order_acq_rel)) {
		return *built.release();
	}
	return *expected;
}

bool Transform::IsWideStable(uint32_t c) const {
	if (!Normalizer::IsInert(options.form, c)) {
		return false;
	}
	if (!CaseMap::IsInvariant(options.case_mapping, options.locale, options.folding, c)) {
		return false;
	}
	return !options.strip_marks || (!Normalizer::IsNonspacingMark(c) && Normalizer::IsInert(strip_form, c));
}

Transform::Classified Transform::Classify(const uint8_t *bytes, size_t size, size_t &position) const {
	uint32_t c = bytes[position];
	if (c < 0x80) {
		position++;
		uint32_t mapped = tables->ascii[c];
		return {c, mapped == Tables::UNSTABLE ? UNSTABLE : mapped};
	}
	if (c >= 0xC2 && c < 0xE0 && position + 1 < size && (bytes[position + 1] & 0xC0) == 0x80) {
		c = ((c & 0x1F) << 6) | (bytes[position + 1] & 0x3F);
		position += 2;
		uint32_t mapped = tables->two_byte[c];
		return {c, mapped == Tables::UNSTABLE ? UNSTABLE : mapped};
	}
	c = DecodeUtf8(bytes, size, position);
	return {c, c == REPLACEMENT_CHARACTER || IsWideStable(c) ? c : UNSTABLE};
}

uint32_t Transform::StableMapping(uint32_t c, TransformBuffer &buffer) const {
	if (whole_value || Normalizer::CombiningClass(c) != 0) {
		return UNSTABLE;
	}
	if (composes && !Normalizer::IsInert(renormalize_form, c)) {
		return UNSTABLE;
	}
	buffer.text.assign(1, c);
	NormalizeInPlace(options.form, buffer);
	if (!composes && (buffer.text.empty() || Normalizer::CombiningClass(buffer.text[0]) != 0)) {
		return UNSTABLE;
	}
	if (sigma_context && ContainsCapitalSigma(buffer.text)) {
		return UNSTABLE;
	}
	MapNormalized(buffer, NO_CONTEXT, NO_CONTEXT);
	if (buffer.text.size() != 1) {
		return UNSTABLE;
	}
	auto result = buffer.text[0];
	if (Normalizer::CombiningClass(result) != 0) {
		return UNSTABLE;
	}
	if (composes && !Normalizer::IsInert(renormalize_form, result)) {
		return UNSTABLE;
	}
	return result;
}

void Transform::MapNormalized(TransformBuffer &buffer, uint32_t before, uint32_t after) const {
	if (options.strip_marks && !options.strip_before_case) {
		MapCase(buffer, before, after);
		StripMarks(buffer);
		return;
	}
	if (options.strip_marks) {
		StripMarks(buffer);
	}
	MapCase(buffer, before, after);
	if (options.case_mapping != CaseMapping::NONE) {
		NormalizeInPlace(renormalize_form, buffer);
	}
}

void Transform::StripMarks(TransformBuffer &buffer) const {
	NormalizeInPlace(strip_form, buffer);
	Normalizer::RemoveNonspacingMarks(buffer.text);
	if (composes) {
		NormalizeInPlace(renormalize_form, buffer);
	}
}

void Transform::MapCase(TransformBuffer &buffer, uint32_t before, uint32_t after) const {
	auto &text = buffer.text;
	auto &scratch = buffer.scratch;
	switch (options.case_mapping) {
	case CaseMapping::NONE:
		break;
	case CaseMapping::LOWER: {
		size_t begin = 0;
		if (before != NO_CONTEXT) {
			text.insert(text.begin(), before);
			begin = 1;
		}
		auto end = text.size();
		if (after != NO_CONTEXT) {
			text.push_back(after);
		}
		CaseMap::ToLower(options.locale, text.data(), text.size(), begin, end, scratch);
		text.swap(scratch);
		break;
	}
	case CaseMapping::UPPER:
		CaseMap::ToUpper(options.locale, text.data(), text.size(), scratch);
		text.swap(scratch);
		break;
	case CaseMapping::FOLD:
		CaseMap::Fold(options.folding, text.data(), text.size(), scratch);
		text.swap(scratch);
		break;
	case CaseMapping::SIMPLE_LOWER:
		for (auto &c : text) {
			c = CaseMap::SimpleLower(c);
		}
		break;
	case CaseMapping::SIMPLE_UPPER:
		for (auto &c : text) {
			c = CaseMap::SimpleUpper(c);
		}
		break;
	}
}

size_t Transform::ApplyWhole(std::string_view input, TransformOutput &output, TransformBuffer &buffer) const {
	Decode(reinterpret_cast<const uint8_t *>(input.data()), 0, input.size(), buffer.text);
	NormalizeInPlace(options.form, buffer);
	MapNormalized(buffer, NO_CONTEXT, NO_CONTEXT);
	size_t needed = 0;
	for (auto c : buffer.text) {
		needed += Utf8Length(c);
	}
	if (needed > output.capacity) {
		output.Grow(0, needed);
	}
	auto out = reinterpret_cast<uint8_t *>(output.data);
	size_t length = 0;
	for (auto c : buffer.text) {
		length += EncodeUtf8(c, out + length);
	}
	return length;
}

size_t Transform::Apply(std::string_view input, TransformOutput &output, TransformBuffer &buffer) const {
	if (whole_value) {
		return ApplyWhole(input, output, buffer);
	}
	auto bytes = reinterpret_cast<const uint8_t *>(input.data());
	auto size = input.size();
	auto out = reinterpret_cast<uint8_t *>(output.data);
	auto capacity = output.capacity;
	auto ascii = tables->ascii;
	auto two_byte = tables->two_byte;
	auto mode = ascii_mode;
	size_t length = 0;
	auto ensure = [&](size_t extra) {
		if (length + extra > capacity) {
			output.Grow(length, length + extra);
			out = reinterpret_cast<uint8_t *>(output.data);
			capacity = output.capacity;
		}
	};
	auto write = [&](uint32_t mapped) {
		ensure(Utf8Length(mapped));
		length += EncodeUtf8(mapped, out + length);
	};
	auto starter = size;
	size_t starter_output = 0;
	size_t position = 0;
	while (position < size) {
		if (mode != AsciiMode::TABLE && position + 8 <= size) {
			uint64_t word;
			memcpy(&word, bytes + position, sizeof(word));
			if ((word & 0x8080808080808080ULL) == 0) {
				ensure(sizeof(word));
				if (mode == AsciiMode::LOWER) {
					word |= UpperMask(word) >> 2;
				} else if (mode == AsciiMode::UPPER) {
					word &= ~(LowerMask(word) >> 2);
				}
				memcpy(out + length, &word, sizeof(word));
				starter = position + 7;
				starter_output = length + 7;
				position += sizeof(word);
				length += sizeof(word);
				continue;
			}
		}
		auto begin = position;
		uint32_t c = bytes[position];
		if (c < 0x80) {
			position++;
			uint32_t mapped = ascii[c];
			if (mapped != Tables::UNSTABLE) {
				starter = begin;
				starter_output = length;
				if (mapped == c) {
					ensure(1);
					out[length++] = static_cast<uint8_t>(c);
				} else {
					write(mapped);
				}
				continue;
			}
		} else if (c >= 0xC2 && c < 0xE0 && position + 1 < size && (bytes[position + 1] & 0xC0) == 0x80) {
			auto trail = bytes[position + 1];
			c = ((c & 0x1F) << 6) | (trail & 0x3F);
			position += 2;
			uint32_t mapped = two_byte[c];
			if (mapped != Tables::UNSTABLE) {
				starter = begin;
				starter_output = length;
				if (mapped == c) {
					ensure(2);
					out[length] = bytes[begin];
					out[length + 1] = trail;
					length += 2;
				} else {
					write(mapped);
				}
				continue;
			}
		} else {
			auto next = position;
			c = DecodeUtf8(bytes, size, next);
			position = next;
			if (c == REPLACEMENT_CHARACTER || IsWideStable(c)) {
				starter = begin;
				starter_output = length;
				if (c == REPLACEMENT_CHARACTER) {
					write(c);
				} else {
					ensure(position - begin);
					for (auto i = begin; i < position; i++) {
						out[length++] = bytes[i];
					}
				}
				continue;
			}
		}
		if (starter != size) {
			begin = starter;
			length = starter_output;
		}
		auto chunk_output = length;
		while (position < size) {
			auto next = position;
			if (Classify(bytes, size, next).mapped != UNSTABLE) {
				break;
			}
			position = next;
		}
		Decode(bytes, begin, position, buffer.text);
		NormalizeInPlace(options.form, buffer);
		auto before = NO_CONTEXT;
		auto after = NO_CONTEXT;
		if (sigma_context && ContainsCapitalSigma(buffer.text)) {
			auto end = position;
			while (position < size) {
				auto next = position;
				auto classified = Classify(bytes, size, next);
				if (classified.mapped != UNSTABLE && !CaseMap::IsCaseIgnorable(classified.code_point)) {
					after = classified.code_point;
					break;
				}
				position = next;
			}
			if (position != end) {
				Decode(bytes, begin, position, buffer.text);
				NormalizeInPlace(options.form, buffer);
			}
			before = PrecedingContext(out, chunk_output);
		}
		MapNormalized(buffer, before, after);
		ensure(4 * buffer.text.size());
		for (auto mapped : buffer.text) {
			length += EncodeUtf8(mapped, out + length);
		}
		starter = size;
	}
	return length;
}

} // namespace text
} // namespace duckdb
