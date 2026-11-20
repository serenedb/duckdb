#include "duckdb/formats/copy_encoding.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/formats/copy_fields.hpp"
#include "duckdb/formats/copy_options.hpp"
#include "simdutf.h"

#include <bit>

namespace duckdb {

namespace {

[[noreturn]] void ThrowInvalidUtf8(std::string_view utf8) {
	ThrowCopyError("22021", CopyInvalidUtf8(utf8));
}

[[noreturn]] void ThrowNotInLatin1(std::string_view utf8) {
	const auto checked = simdutf::validate_utf8_with_errors(utf8.data(), utf8.size());
	const idx_t valid = checked.error == simdutf::error_code::SUCCESS ? utf8.size() : checked.count;
	for (idx_t at = 0; at < valid;) {
		const auto lead = static_cast<uint8_t>(utf8[at]);
		const idx_t length = Utf8SequenceLength(lead);
		if (length > 2 || (length == 2 && lead > 0xC3)) {
			string bytes;
			for (idx_t i = at; i < at + length; i++) {
				bytes += StringUtil::Format("%s0x%02x", i == at ? "" : " ", static_cast<uint8_t>(utf8[i]));
			}
			ThrowCopyError("22P05", "character with byte sequence " + bytes +
			                            " in encoding \"UTF8\" has no equivalent in encoding \"LATIN1\"");
		}
		at += length;
	}
	ThrowInvalidUtf8(utf8);
}

template <typename UNIT>
void AppendUnits(UNIT *units, idx_t count, bool swap, string &out) {
	if (swap) {
		for (idx_t i = 0; i < count; i++) {
			units[i] = std::byteswap(units[i]);
		}
	}
	out.append(reinterpret_cast<const char *>(units), count * sizeof(UNIT));
}

} // namespace

std::optional<TextEncoding> CopyOutputEncoding(const string &name) {
	if (name == "utf-8") {
		return std::nullopt;
	}
	if (name == "latin-1") {
		return TextEncoding::LATIN1;
	}
	if (name == "utf-16le") {
		return TextEncoding::UTF16_LE;
	}
	if (name == "utf-16be") {
		return TextEncoding::UTF16_BE;
	}
	if (name == "utf-32le") {
		return TextEncoding::UTF32_LE;
	}
	if (name == "utf-32be") {
		return TextEncoding::UTF32_BE;
	}
	throw InternalException("COPY has no encoder for \"%s\"", name);
}

CopyEncoder::CopyEncoder(TextEncoding encoding_p) : encoding(encoding_p) {
}

void CopyEncoder::Encode(std::string_view input, string &out) {
	string joined;
	if (!carry.empty()) {
		joined = carry;
		joined.append(input);
		input = joined;
	}
	idx_t end = input.size();
	idx_t start = end;
	while (start != 0 && end - start < 4 && (static_cast<uint8_t>(input[start - 1]) & 0xC0) == 0x80) {
		start--;
	}
	if (start != 0 && Utf8SequenceLength(static_cast<uint8_t>(input[start - 1])) > end - start + 1) {
		end = start - 1;
	}
	const auto data = input.substr(0, end);
	carry.assign(input.substr(end));
	if (data.empty()) {
		return;
	}
	switch (encoding) {
	case TextEncoding::LATIN1: {
		const idx_t old = out.size();
		out.resize(old + data.size());
		const auto written = simdutf::convert_utf8_to_latin1(data.data(), data.size(), &out[old]);
		if (written == 0) {
			ThrowNotInLatin1(data);
		}
		out.resize(old + written);
		return;
	}
	case TextEncoding::UTF16_LE:
	case TextEncoding::UTF16_BE: {
		std::u16string units(simdutf::utf16_length_from_utf8(data.data(), data.size()), u'\0');
		const auto written = encoding == TextEncoding::UTF16_BE
		                         ? simdutf::convert_utf8_to_utf16be(data.data(), data.size(), units.data())
		                         : simdutf::convert_utf8_to_utf16le(data.data(), data.size(), units.data());
		if (written == 0) {
			ThrowInvalidUtf8(data);
		}
		AppendUnits(units.data(), written, false, out);
		return;
	}
	case TextEncoding::UTF32_LE:
	case TextEncoding::UTF32_BE: {
		std::u32string units(simdutf::utf32_length_from_utf8(data.data(), data.size()), U'\0');
		const auto written = simdutf::convert_utf8_to_utf32(data.data(), data.size(), units.data());
		if (written == 0) {
			ThrowInvalidUtf8(data);
		}
		const bool big = encoding == TextEncoding::UTF32_BE;
		AppendUnits(units.data(), written, big != (std::endian::native == std::endian::big), out);
		return;
	}
	}
}

} // namespace duckdb
