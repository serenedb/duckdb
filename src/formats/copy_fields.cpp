#include "duckdb/formats/copy_fields.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/formats/csv_simd.hpp"
#include "simdutf.h"

#include <bit>

namespace duckdb {

namespace {

constexpr const char *SQLSTATE_BAD_COPY_FILE_FORMAT = "22P04";
constexpr idx_t PRINTOUT_BYTES = 100;

int HexDigit(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

bool OctalDigit(char c) {
	return c >= '0' && c <= '7';
}

bool IsEndMarkerPrefix(std::string_view bytes) {
	return bytes == "\\" || bytes == "\\." || bytes == "\\.\r";
}

} // namespace

static void VerifyEscapedBytes(std::string_view value) {
	if (!simdutf::validate_utf8(value.data(), value.size()) || value.find('\0') != std::string_view::npos) {
		ThrowCopyError("22021", CopyInvalidUtf8(value));
	}
}

std::string_view UnescapePostgresText(std::string_view field, char *out, bool &escaped_high_or_nul) {
	char *const begin = out;
	escaped_high_or_nul = false;
	for (idx_t i = 0; i < field.size();) {
		const char c = field[i++];
		if (c != '\\') {
			*out++ = c;
			continue;
		}
		if (i == field.size()) {
			*out++ = '\\';
			break;
		}
		const char e = field[i++];
		switch (e) {
		case 'b':
			*out++ = '\b';
			break;
		case 'f':
			*out++ = '\f';
			break;
		case 'n':
			*out++ = '\n';
			break;
		case 'r':
			*out++ = '\r';
			break;
		case 't':
			*out++ = '\t';
			break;
		case 'v':
			*out++ = '\v';
			break;
		case 'x': {
			int value = 0;
			idx_t digits = 0;
			for (; digits < 2 && i < field.size() && HexDigit(field[i]) >= 0; digits++, i++) {
				value = value * 16 + HexDigit(field[i]);
			}
			escaped_high_or_nul |= digits > 0 && (value == 0 || value >= 0x80);
			*out++ = digits == 0 ? 'x' : static_cast<char>(value);
			break;
		}
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7': {
			int value = e - '0';
			for (idx_t digits = 1; digits < 3 && i < field.size() && OctalDigit(field[i]); digits++, i++) {
				value = value * 8 + (field[i] - '0');
			}
			value &= 0xFF;
			escaped_high_or_nul |= value == 0 || value >= 0x80;
			*out++ = static_cast<char>(value);
			break;
		}
		case '.':
			ThrowCopyError(SQLSTATE_BAD_COPY_FILE_FORMAT, "end-of-copy marker is not alone on its line");
		default:
			*out++ = e;
			break;
		}
	}
	return {begin, static_cast<size_t>(out - begin)};
}

std::string_view UnquotePostgresCsv(std::string_view field, char quote, char escape, char *out) {
	char *const begin = out;
	idx_t i = 0;
	while (i < field.size()) {
		const char c = field[i++];
		if (c != quote) {
			*out++ = c;
			continue;
		}
		while (i < field.size()) {
			const char q = field[i++];
			if (q == escape && i < field.size() && (field[i] == escape || field[i] == quote)) {
				*out++ = field[i++];
				continue;
			}
			if (q == quote) {
				break;
			}
			*out++ = q;
		}
	}
	return {begin, static_cast<size_t>(out - begin)};
}

CopyFieldKind ReadCopyField(const CopyFromRules &rules, idx_t column, std::string_view raw, char *out,
                            std::string_view &value) {
	auto &options = rules.options;
	if (rules.Text()) {
		if (raw == options.null_str) {
			return CopyFieldKind::NULL_VALUE;
		}
		if (options.has_default && raw == options.default_str) {
			return CopyFieldKind::DEFAULT_VALUE;
		}
		value = raw;
		if (raw.find('\\') != std::string_view::npos) {
			bool escaped_high_or_nul;
			value = UnescapePostgresText(raw, out, escaped_high_or_nul);
			if (escaped_high_or_nul) {
				VerifyEscapedBytes(value);
			}
		}
		return CopyFieldKind::VALUE;
	}
	if (raw.find(options.quote) == std::string_view::npos) {
		const bool force_not_null = !rules.force_not_null.empty() && rules.force_not_null[column];
		if (!force_not_null && raw == options.null_str) {
			return CopyFieldKind::NULL_VALUE;
		}
		if (options.has_default && raw == options.default_str) {
			return CopyFieldKind::DEFAULT_VALUE;
		}
		value = raw;
		return CopyFieldKind::VALUE;
	}
	value = UnquotePostgresCsv(raw, options.quote, options.escape, out);
	if (!rules.force_null.empty() && rules.force_null[column] && value == options.null_str) {
		return CopyFieldKind::NULL_VALUE;
	}
	return CopyFieldKind::VALUE;
}

CopyLineBreakFinder::CopyLineBreakFinder(const CopyFormatOptions &options)
    : csv(options.format == CopyFormat::CSV), quote(options.quote), escape(options.escape),
      quoted_escape(options.escape != options.quote) {
}

char CopyLineBreakFinder::Feed(std::string_view bytes) {
	if (bytes.empty()) {
		return '\0';
	}
	if (after_cr) {
		return bytes[0] == '\n' ? '\n' : '\r';
	}
	idx_t i = 0;
	if (after_backslash) {
		after_backslash = false;
		i = 1;
	} else if (after_escape) {
		after_escape = false;
		i = bytes[0] == quote || bytes[0] == escape ? 1 : 0;
	}
	const char opener = csv ? quote : '\\';
	const char escaper = csv && quoted_escape ? escape : opener;
	for (; i < bytes.size(); i++) {
		if (i + CSV_BLOCK_BYTES <= bytes.size()) {
			const uint64_t found = CsvVector(bytes.data() + i).EqualAny('\n', '\r', opener, escaper);
			if (!found) {
				i += CSV_BLOCK_BYTES - 1;
				continue;
			}
			i += std::countr_zero(found);
		}
		const char c = bytes[i];
		if (csv && in_quote) {
			if (quoted_escape && c == escape) {
				if (i + 1 == bytes.size()) {
					after_escape = true;
				} else if (bytes[i + 1] == quote || bytes[i + 1] == escape) {
					i++;
				}
			} else if (c == quote) {
				in_quote = false;
			}
			continue;
		}
		if (csv && c == quote) {
			in_quote = true;
			continue;
		}
		if (!csv && c == '\\') {
			if (i + 1 == bytes.size()) {
				after_backslash = true;
			}
			i++;
			continue;
		}
		if (c == '\n') {
			return '\n';
		}
		if (c == '\r') {
			if (i + 1 < bytes.size()) {
				return bytes[i + 1] == '\n' ? '\n' : '\r';
			}
			after_cr = true;
		}
	}
	return '\0';
}

char CopyLineBreakFinder::Finish() const {
	return after_cr ? '\r' : '\n';
}

CopyEndScan CopyEndMarkerScanner::Scan(std::string_view chunk) {
	CopyEndScan result;
	if (ended) {
		return result;
	}
	while (!pending.empty() && !chunk.empty()) {
		pending.push_back(chunk.front());
		if (pending == "\\.\n" || pending == "\\.\r\n") {
			ended = true;
			pending.clear();
			return result;
		}
		if (IsEndMarkerPrefix(pending)) {
			chunk.remove_prefix(1);
			continue;
		}
		pending.pop_back();
		escape_carry = pending == "\\" ? 1 : 0;
		at_line_start = false;
		carry = std::move(pending);
		pending.clear();
		result.carry = carry;
		break;
	}
	if (!pending.empty()) {
		return result;
	}
	result.data = ScanChunk(chunk);
	return result;
}

std::string_view CopyEndMarkerScanner::Finish() {
	if (ended || pending == "\\.") {
		ended = true;
		pending.clear();
		return std::string_view();
	}
	carry = std::move(pending);
	pending.clear();
	return carry;
}

std::string_view CopyEndMarkerScanner::Hold(std::string_view chunk, idx_t at) {
	pending = string(chunk.substr(at));
	return chunk.substr(0, at);
}

std::string_view CopyEndMarkerScanner::ScanChunk(std::string_view chunk) {
	const char *data = chunk.data();
	const idx_t size = chunk.size();
	char padded[CSV_BLOCK_BYTES];
	for (idx_t block = 0; block < size; block += CSV_BLOCK_BYTES) {
		const idx_t available = MinValue<idx_t>(size - block, CSV_BLOCK_BYTES);
		const char *bytes = data + block;
		if (available < CSV_BLOCK_BYTES) {
			memset(padded, 0, sizeof(padded));
			memcpy(padded, bytes, available);
			bytes = padded;
		}
		const CsvVector vector(bytes);
		const uint64_t backslashes = vector.Equal('\\');
		const uint64_t escaped = CsvFindEscaped(backslashes, escape_carry);
		uint64_t candidates = backslashes & ~escaped;
		if (available < CSV_BLOCK_BYTES) {
			escape_carry = (candidates >> (available - 1)) & 1;
		}
		const bool line_start = at_line_start;
		at_line_start = bytes[available - 1] == '\n' && !((escaped >> (available - 1)) & 1);
		if (!candidates) {
			continue;
		}
		candidates &= ((vector.Equal('\n') & ~escaped) << 1) | uint64_t(line_start);
		for (; candidates; candidates &= candidates - 1) {
			const idx_t at = block + NumericCast<idx_t>(std::countr_zero(candidates));
			if (at + 1 == size || (data[at + 1] == '.' && at + 2 == size)) {
				return Hold(chunk, at);
			}
			if (data[at + 1] != '.') {
				continue;
			}
			if (data[at + 2] == '\r') {
				if (at + 3 == size) {
					return Hold(chunk, at);
				}
				if (data[at + 3] != '\n') {
					continue;
				}
			} else if (data[at + 2] != '\n') {
				continue;
			}
			ended = true;
			return chunk.substr(0, at);
		}
	}
	return chunk;
}

string CopyPrintout(std::string_view value) {
	if (value.size() <= PRINTOUT_BYTES) {
		return string(value);
	}
	idx_t size = PRINTOUT_BYTES;
	while (size != 0 && (static_cast<uint8_t>(value[size]) & 0xC0) == 0x80) {
		size--;
	}
	return string(value.substr(0, size)) + "...";
}

idx_t Utf8SequenceLength(uint8_t lead) {
	if (lead >= 0xF0 && lead < 0xF8) {
		return 4;
	}
	if (lead >= 0xE0 && lead < 0xF0) {
		return 3;
	}
	return lead >= 0xC0 && lead < 0xE0 ? 2 : 1;
}

string CopyInvalidUtf8(std::string_view bytes) {
	const auto checked = simdutf::validate_utf8_with_errors(bytes.data(), bytes.size());
	const idx_t invalid = checked.error == simdutf::error_code::SUCCESS ? bytes.size() : checked.count;
	const auto nul = bytes.substr(0, invalid).find('\0');
	const idx_t offset = nul != std::string_view::npos ? nul : invalid < bytes.size() ? invalid : 0;
	const idx_t length = nul == std::string_view::npos && offset < bytes.size()
	                         ? Utf8SequenceLength(static_cast<uint8_t>(bytes[offset]))
	                         : 1;
	string message = "invalid byte sequence for encoding \"UTF8\":";
	for (idx_t at = offset; at < MinValue<idx_t>(offset + length, bytes.size()); at++) {
		message += StringUtil::Format(" 0x%02x", static_cast<uint8_t>(bytes[at]));
	}
	return message;
}

string CopyContext(const string &table, idx_t line, const string &column, bool has_value, const string &value) {
	auto context = "COPY " + table + ", line " + std::to_string(line);
	if (!column.empty()) {
		context += ", column " + column;
	}
	if (has_value) {
		context += ": \"" + value + "\"";
	}
	return context;
}

} // namespace duckdb
