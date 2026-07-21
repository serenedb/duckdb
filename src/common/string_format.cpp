#include "duckdb/common/string_format.hpp"

#include "duckdb/common/exception.hpp"
#include "fmt/format.h"

#include <iterator>
#include <locale>

namespace duckdb {

namespace {

using native_int128_t = __int128;
using native_uint128_t = unsigned __int128;

enum class FormatAlign : uint8_t { NONE, LEFT, RIGHT, CENTER, NUMERIC };
enum class FormatSign : uint8_t { NONE, PLUS, SPACE };
enum class PrintfLength : uint8_t { NONE, CHAR, SHORT, LONG, LONG_LONG, INTMAX, SIZE, PTRDIFF, LONG_DOUBLE };

struct PrintfSpecs {
	char fill = ' ';
	FormatAlign align = FormatAlign::RIGHT;
	FormatSign sign = FormatSign::NONE;
	bool alt = false;
	char thousands = '\0';
	int32_t width = 0;
	int32_t precision = -1;
	char type = '\0';
};

struct PrintfInteger {
	native_uint128_t bits;
	bool is_signed;
};

native_uint128_t HugeintBits(hugeint_t value) {
	return (static_cast<native_uint128_t>(static_cast<uint64_t>(value.upper)) << 64) | value.lower;
}

native_uint128_t UhugeintBits(uhugeint_t value) {
	return (static_cast<native_uint128_t>(value.upper) << 64) | value.lower;
}

native_uint128_t SignExtend(native_uint128_t bits, idx_t size) {
	auto shift = 128 - size * 8;
	return static_cast<native_uint128_t>(static_cast<native_int128_t>(bits << shift) >> shift);
}

native_uint128_t Truncate(native_uint128_t bits, idx_t size) {
	return size >= 16 ? bits : bits & ((static_cast<native_uint128_t>(1) << (size * 8)) - 1);
}

bool IsFormatDigit(char c) {
	return c >= '0' && c <= '9';
}

bool IsFormatNameStart(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool IsFloatPresentation(char type) {
	switch (type) {
	case 'a':
	case 'A':
	case 'e':
	case 'E':
	case 'f':
	case 'F':
	case 'g':
	case 'G':
		return true;
	default:
		return false;
	}
}

bool IsIntegerArgument(const FormatArgument &arg) {
	switch (arg.type) {
	case FormatArgumentType::BOOLEAN:
	case FormatArgumentType::BIGINT:
	case FormatArgumentType::UBIGINT:
	case FormatArgumentType::HUGEINT:
	case FormatArgumentType::UHUGEINT:
		return true;
	default:
		return false;
	}
}

PrintfInteger IntegerArgumentValue(const FormatArgument &arg) {
	switch (arg.type) {
	case FormatArgumentType::BOOLEAN:
		return {arg.boolean ? 1u : 0u, false};
	case FormatArgumentType::BIGINT:
		return {static_cast<native_uint128_t>(static_cast<native_int128_t>(arg.bigint)), true};
	case FormatArgumentType::UBIGINT:
		return {arg.ubigint, false};
	case FormatArgumentType::HUGEINT:
		return {HugeintBits(arg.hugeint), true};
	case FormatArgumentType::UHUGEINT:
		return {UhugeintBits(arg.uhugeint), false};
	default:
		throw InternalException("not an integer format argument");
	}
}

idx_t IntegerArgumentSize(const FormatArgument &arg) {
	switch (arg.type) {
	case FormatArgumentType::BOOLEAN:
		return 1;
	case FormatArgumentType::HUGEINT:
	case FormatArgumentType::UHUGEINT:
		return 16;
	default:
		return 8;
	}
}

PrintfInteger ConvertPrintfInteger(const FormatArgument &arg, PrintfLength length, char type) {
	auto value = IntegerArgumentValue(arg);
	auto arg_is_unsigned = arg.type == FormatArgumentType::UBIGINT || arg.type == FormatArgumentType::UHUGEINT;
	if ((type == 'd' || type == 'i') && arg_is_unsigned) {
		type = 'u';
	}
	auto arg_is_signed = arg.type == FormatArgumentType::BIGINT || arg.type == FormatArgumentType::HUGEINT;
	auto is_signed = type == 'd' || type == 'i' || (IsFloatPresentation(type) && arg_is_signed);
	auto target_is_bool = length == PrintfLength::NONE && arg.type == FormatArgumentType::BOOLEAN;
	idx_t target_size;
	switch (length) {
	case PrintfLength::NONE:
		target_size = IntegerArgumentSize(arg);
		break;
	case PrintfLength::CHAR:
		target_size = 1;
		break;
	case PrintfLength::SHORT:
		target_size = 2;
		break;
	default:
		target_size = 8;
		break;
	}
	if (target_size <= 4) {
		auto bits = target_is_bool ? native_uint128_t(value.bits != 0) : Truncate(value.bits, target_size);
		if (is_signed) {
			return {target_is_bool ? bits : SignExtend(bits, target_size), true};
		}
		return {bits, false};
	}
	if (is_signed) {
		if (target_size > 8) {
			return {value.bits, true};
		}
		return {SignExtend(Truncate(value.bits, 8), 8), true};
	}
	if (arg.type == FormatArgumentType::BOOLEAN) {
		return {native_uint128_t(value.bits != 0), false};
	}
	return {Truncate(value.bits, IntegerArgumentSize(arg)), false};
}

void AppendDigits(string &out, native_uint128_t value, uint32_t base, bool upper) {
	char buffer[128];
	auto digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	idx_t size = 0;
	do {
		buffer[size++] = digits[static_cast<uint32_t>(value % base)];
		value /= base;
	} while (value != 0);
	while (size > 0) {
		out += buffer[--size];
	}
}

void AppendGroupedDecimal(string &out, native_uint128_t value, char separator) {
	string digits;
	AppendDigits(digits, value, 10, false);
	for (idx_t i = 0; i < digits.size(); i++) {
		if (i > 0 && (digits.size() - i) % 3 == 0) {
			out += separator;
		}
		out += digits[i];
	}
}

template <class WRITE>
void AppendPadded(string &out, const PrintfSpecs &specs, idx_t used, WRITE &&write) {
	auto width = static_cast<idx_t>(specs.width);
	if (width <= used) {
		write(out);
		return;
	}
	auto padding = width - used;
	if (specs.align == FormatAlign::RIGHT) {
		out.append(padding, specs.fill);
		write(out);
	} else if (specs.align == FormatAlign::CENTER) {
		auto left = padding / 2;
		out.append(left, specs.fill);
		write(out);
		out.append(padding - left, specs.fill);
	} else {
		write(out);
		out.append(padding, specs.fill);
	}
}

void AppendPrintfInteger(string &out, PrintfInteger value, const PrintfSpecs &specs) {
	auto negative = value.is_signed && static_cast<native_int128_t>(value.bits) < 0;
	auto abs_value = negative ? ~value.bits + 1 : value.bits;
	string prefix;
	if (negative) {
		prefix += '-';
	} else if (specs.sign == FormatSign::PLUS) {
		prefix += '+';
	} else if (specs.sign == FormatSign::SPACE) {
		prefix += ' ';
	}
	string digits;
	if (specs.thousands != '\0') {
		AppendGroupedDecimal(digits, abs_value, specs.thousands);
	} else {
		switch (specs.type) {
		case '\0':
		case 'd':
		case 'n':
		case 'l':
		case 'L':
			AppendDigits(digits, abs_value, 10, false);
			break;
		case 'x':
		case 'X':
			if (specs.alt) {
				prefix += '0';
				prefix += specs.type;
			}
			AppendDigits(digits, abs_value, 16, specs.type == 'X');
			break;
		case 'b':
		case 'B':
			if (specs.alt) {
				prefix += '0';
				prefix += specs.type;
			}
			AppendDigits(digits, abs_value, 2, false);
			break;
		case 'o':
			AppendDigits(digits, abs_value, 8, false);
			if (specs.alt && specs.precision <= static_cast<int32_t>(digits.size()) && abs_value != 0) {
				prefix += '0';
			}
			break;
		default:
			throw InvalidInputException(string("Invalid type specifier \"") + specs.type +
			                            "\" for formatting a value of type int");
		}
	}
	auto num_digits = digits.size();
	auto size = prefix.size() + num_digits;
	auto fill = specs.fill;
	idx_t padding = 0;
	if (specs.align == FormatAlign::NUMERIC) {
		if (static_cast<idx_t>(specs.width) > size) {
			padding = static_cast<idx_t>(specs.width) - size;
			size = static_cast<idx_t>(specs.width);
		}
	} else if (specs.precision > static_cast<int32_t>(num_digits)) {
		size = prefix.size() + static_cast<idx_t>(specs.precision);
		padding = static_cast<idx_t>(specs.precision) - num_digits;
		fill = '0';
	}
	AppendPadded(out, specs, size, [&](string &target) {
		target += prefix;
		target.append(padding, fill);
		target += digits;
	});
}

void AppendPrintfString(string &out, std::string_view value, const PrintfSpecs &specs) {
	if (specs.precision >= 0 && static_cast<idx_t>(specs.precision) < value.size()) {
		value = value.substr(0, static_cast<idx_t>(specs.precision));
	}
	idx_t code_points = 0;
	for (auto c : value) {
		if ((static_cast<uint8_t>(c) & 0xC0) != 0x80) {
			code_points++;
		}
	}
	AppendPadded(out, specs, code_points, [&](string &target) { target += value; });
}

class GroupingPunct : public std::numpunct<char> {
public:
	GroupingPunct(char thousands, char decimal) : thousands(thousands), decimal(decimal) {
	}

protected:
	char do_thousands_sep() const override {
		return thousands;
	}
	char do_decimal_point() const override {
		return decimal;
	}
	std::string do_grouping() const override {
		return "\3";
	}

private:
	char thousands;
	char decimal;
};

std::locale MakeGroupingLocale(char separator) {
	return std::locale(std::locale::classic(), new GroupingPunct(separator, separator == '.' ? ',' : '.'));
}

std::locale GroupingLocale(char separator) {
	switch (separator) {
	case '\0':
		return std::locale::classic();
	case ',': {
		static const std::locale comma = MakeGroupingLocale(',');
		return comma;
	}
	case '_': {
		static const std::locale underscore = MakeGroupingLocale('_');
		return underscore;
	}
	case '\'': {
		static const std::locale quote = MakeGroupingLocale('\'');
		return quote;
	}
	case '.': {
		static const std::locale dot = MakeGroupingLocale('.');
		return dot;
	}
	default:
		return MakeGroupingLocale(separator);
	}
}

template <class FUNC>
auto WithFmtErrors(FUNC &&func) -> decltype(func()) {
	try {
		return func();
	} catch (fmt::format_error &ex) {
		throw InvalidInputException(ex.what());
	}
}

void AppendPrintfDouble(string &out, double value, const PrintfSpecs &specs) {
	auto type = specs.type;
	auto localized = specs.thousands != '\0';
	if (type == 'n') {
		type = '\0';
		localized = true;
	} else if (!IsFloatPresentation(type)) {
		throw InvalidInputException(string("Invalid type specifier \"") + specs.type +
		                            "\" for formatting a value of type float");
	}
	string spec = "{:";
	if (specs.align == FormatAlign::LEFT) {
		spec += '<';
	}
	if (specs.sign == FormatSign::PLUS) {
		spec += '+';
	} else if (specs.sign == FormatSign::SPACE) {
		spec += ' ';
	}
	if (specs.alt) {
		spec += '#';
	}
	if (specs.align == FormatAlign::NUMERIC) {
		spec += '0';
	}
	if (specs.width > 0) {
		spec += std::to_string(specs.width);
	}
	if (specs.precision >= 0) {
		spec += '.';
		spec += std::to_string(specs.precision);
	}
	if (localized) {
		spec += 'L';
	}
	if (type != '\0') {
		spec += type;
	}
	spec += '}';
	WithFmtErrors([&]() {
		fmt::vformat_to(std::back_inserter(out), GroupingLocale(specs.thousands), fmt::string_view(spec),
		                fmt::make_format_args(value));
	});
}

class FormatArgumentIds {
public:
	explicit FormatArgumentIds(idx_t count) : count(count) {
	}

	idx_t Next() {
		if (next < 0) {
			throw InvalidInputException("cannot switch from manual to automatic argument indexing");
		}
		return Check(static_cast<idx_t>(next++));
	}

	idx_t Manual(idx_t id) {
		if (next > 0) {
			throw InvalidInputException("cannot switch from automatic to manual argument indexing");
		}
		next = -1;
		return Check(id);
	}

private:
	idx_t Check(idx_t id) const {
		if (id >= count) {
			throw InvalidInputException("Argument index \"" + std::to_string(id) + "\" out of range");
		}
		return id;
	}

	idx_t count;
	int64_t next = 0;
};

int32_t ParseFormatInt(std::string_view format, idx_t &pos) {
	constexpr uint32_t max_int = static_cast<uint32_t>(NumericLimits<int32_t>::Maximum());
	uint32_t value = 0;
	do {
		if (value > max_int / 10) {
			value = max_int + 1;
			break;
		}
		value = value * 10 + static_cast<uint32_t>(format[pos] - '0');
		pos++;
	} while (pos < format.size() && IsFormatDigit(format[pos]));
	if (value > max_int) {
		throw InvalidInputException("number is too big");
	}
	return static_cast<int32_t>(value);
}

int32_t PrintfWidthValue(const FormatArgument &arg, PrintfSpecs &specs) {
	if (!IsIntegerArgument(arg)) {
		throw InvalidInputException("width is not integer");
	}
	auto value = IntegerArgumentValue(arg);
	auto negative = value.is_signed && static_cast<native_int128_t>(value.bits) < 0;
	auto abs_value = negative ? ~value.bits + 1 : value.bits;
	if (abs_value > static_cast<native_uint128_t>(NumericLimits<int32_t>::Maximum())) {
		throw InvalidInputException("number is too big");
	}
	if (negative) {
		specs.align = FormatAlign::LEFT;
	}
	return static_cast<int32_t>(abs_value);
}

int32_t PrintfPrecisionValue(const FormatArgument &arg) {
	if (!IsIntegerArgument(arg)) {
		throw InvalidInputException("precision is not integer");
	}
	auto value = IntegerArgumentValue(arg);
	if (value.is_signed) {
		auto signed_value = static_cast<native_int128_t>(value.bits);
		if (signed_value < NumericLimits<int32_t>::Minimum() || signed_value > NumericLimits<int32_t>::Maximum()) {
			throw InvalidInputException("number is too big");
		}
		return signed_value < 0 ? 0 : static_cast<int32_t>(signed_value);
	}
	if (value.bits > static_cast<native_uint128_t>(NumericLimits<int32_t>::Maximum())) {
		throw InvalidInputException("number is too big");
	}
	return static_cast<int32_t>(value.bits);
}

void ParsePrintfFlags(std::string_view format, idx_t &pos, PrintfSpecs &specs) {
	for (; pos < format.size(); pos++) {
		switch (format[pos]) {
		case '-':
			specs.align = FormatAlign::LEFT;
			break;
		case '+':
			specs.sign = FormatSign::PLUS;
			break;
		case '0':
			specs.fill = '0';
			break;
		case ' ':
			specs.sign = FormatSign::SPACE;
			break;
		case '#':
			specs.alt = true;
			break;
		case ',':
		case '\'':
		case '_':
			specs.thousands = format[pos];
			break;
		default:
			return;
		}
	}
}

int64_t ParsePrintfHeader(std::string_view format, idx_t &pos, PrintfSpecs &specs, const vector<FormatArgument> &args,
                          FormatArgumentIds &ids) {
	int64_t arg_index = -1;
	auto c = format[pos];
	if (IsFormatDigit(c)) {
		auto value = ParseFormatInt(format, pos);
		if (pos < format.size() && format[pos] == '$') {
			pos++;
			arg_index = value;
		} else {
			if (c == '0') {
				specs.fill = '0';
			}
			if (value != 0) {
				specs.width = value;
				return arg_index;
			}
		}
	}
	ParsePrintfFlags(format, pos, specs);
	if (pos < format.size()) {
		if (IsFormatDigit(format[pos])) {
			specs.width = ParseFormatInt(format, pos);
		} else if (format[pos] == '*') {
			pos++;
			specs.width = PrintfWidthValue(args[ids.Next()], specs);
		}
	}
	return arg_index;
}

PrintfLength ParsePrintfLength(std::string_view format, idx_t &pos) {
	if (pos >= format.size()) {
		return PrintfLength::NONE;
	}
	auto next = pos + 1 < format.size() ? format[pos + 1] : '\0';
	switch (format[pos]) {
	case 'h':
		pos++;
		if (next == 'h') {
			pos++;
			return PrintfLength::CHAR;
		}
		return PrintfLength::SHORT;
	case 'l':
		pos++;
		if (next == 'l') {
			pos++;
			return PrintfLength::LONG_LONG;
		}
		return PrintfLength::LONG;
	case 'j':
		pos++;
		return PrintfLength::INTMAX;
	case 'z':
		pos++;
		return PrintfLength::SIZE;
	case 't':
		pos++;
		return PrintfLength::PTRDIFF;
	case 'L':
		pos++;
		return PrintfLength::LONG_DOUBLE;
	default:
		return PrintfLength::NONE;
	}
}

void AppendPrintfArgument(string &out, const FormatArgument &arg, PrintfLength length, bool empty_precision,
                          PrintfSpecs &specs) {
	if (!IsIntegerArgument(arg)) {
		if (arg.type == FormatArgumentType::DOUBLE) {
			AppendPrintfDouble(out, arg.floating, specs);
			return;
		}
		if (specs.type != 's') {
			throw InvalidInputException(string("Invalid type specifier \"") + specs.type +
			                            "\" for formatting a value of type string");
		}
		AppendPrintfString(out, arg.string, specs);
		return;
	}
	if (arg.type == FormatArgumentType::BOOLEAN && specs.type == 's') {
		AppendPrintfString(out, arg.boolean ? "true" : "false", specs);
		return;
	}
	auto value =
	    length == PrintfLength::LONG_DOUBLE ? IntegerArgumentValue(arg) : ConvertPrintfInteger(arg, length, specs.type);
	switch (specs.type) {
	case 'i':
	case 'u':
		specs.type = 'd';
		break;
	case 'c': {
		auto c = static_cast<char>(static_cast<uint32_t>(value.bits));
		auto char_specs = specs;
		char_specs.sign = FormatSign::NONE;
		char_specs.alt = false;
		char_specs.align = FormatAlign::RIGHT;
		AppendPadded(out, char_specs, 1, [&](string &target) { target += c; });
		return;
	}
	default:
		break;
	}
	if (specs.type == 'd' && empty_precision) {
		specs.thousands = '.';
	}
	AppendPrintfInteger(out, value, specs);
}

fmt::basic_format_arg<fmt::format_context> ToFmtArgument(const FormatArgument &arg) {
	switch (arg.type) {
	case FormatArgumentType::BOOLEAN:
		return fmt::basic_format_arg<fmt::format_context>(arg.boolean);
	case FormatArgumentType::BIGINT:
		return fmt::basic_format_arg<fmt::format_context>(static_cast<long long>(arg.bigint));
	case FormatArgumentType::UBIGINT:
		return fmt::basic_format_arg<fmt::format_context>(static_cast<unsigned long long>(arg.ubigint));
	case FormatArgumentType::HUGEINT:
		return fmt::basic_format_arg<fmt::format_context>(static_cast<native_int128_t>(HugeintBits(arg.hugeint)));
	case FormatArgumentType::UHUGEINT:
		return fmt::basic_format_arg<fmt::format_context>(UhugeintBits(arg.uhugeint));
	case FormatArgumentType::DOUBLE:
		return fmt::basic_format_arg<fmt::format_context>(arg.floating);
	default:
		return fmt::basic_format_arg<fmt::format_context>(fmt::string_view(arg.string.data(), arg.string.size()));
	}
}

idx_t ParseFormatArgId(std::string_view format, idx_t &pos, FormatArgumentIds &ids) {
	auto c = format[pos];
	if (c == '}' || c == ':') {
		return ids.Next();
	}
	if (IsFormatDigit(c)) {
		auto index = ParseFormatInt(format, pos);
		if (pos == format.size() || (format[pos] != '}' && format[pos] != ':')) {
			throw InvalidInputException("invalid format string");
		}
		return ids.Manual(static_cast<idx_t>(index));
	}
	if (!IsFormatNameStart(c)) {
		throw InvalidInputException("invalid format string");
	}
	auto start = pos;
	do {
		pos++;
	} while (pos < format.size() && (IsFormatNameStart(format[pos]) || IsFormatDigit(format[pos])));
	auto name = string(format.substr(start, pos - start));
	throw InvalidInputException("Argument with name \"" + name +
	                            "\" not found, did you mean to use it as a format specifier (e.g. {:" + name + "})");
}

void ParseFormatDynamicSpec(std::string_view format, idx_t &pos, FormatArgumentIds &ids, string &spec) {
	pos++;
	if (pos == format.size()) {
		throw InvalidInputException("invalid format string");
	}
	auto id = ParseFormatArgId(format, pos, ids);
	if (pos == format.size() || format[pos] != '}') {
		throw InvalidInputException("invalid format string");
	}
	pos++;
	spec += '{';
	spec += std::to_string(id);
	spec += '}';
}

void AppendFormatField(string &out, std::string_view format, idx_t &pos, const vector<FormatArgument> &args,
                       fmt::format_args fmt_args, FormatArgumentIds &ids) {
	auto arg_index = ParseFormatArgId(format, pos, ids);
	auto &arg = args[arg_index];
	if (pos == format.size() || (format[pos] != '}' && format[pos] != ':')) {
		throw InvalidInputException("missing '}' in format string");
	}
	string spec;
	char separator = '\0';
	char type = '\0';
	if (format[pos] == ':') {
		pos++;
		if (pos < format.size() && format[pos] != '}') {
			for (idx_t i = pos + 1 < format.size() ? 1 : 0;; i--) {
				auto c = format[pos + i];
				if (c == '<' || c == '>' || c == '=' || c == '^') {
					if (i > 0 && format[pos] == '{') {
						throw InvalidInputException("invalid fill character '{'");
					}
					spec += format.substr(pos, i + 1);
					pos += i + 1;
					break;
				}
				if (i == 0) {
					break;
				}
			}
			if (pos < format.size()) {
				switch (format[pos]) {
				case '+':
				case '-':
				case ' ':
					spec += format[pos++];
					break;
				case ',':
				case '_':
				case '\'':
					separator = format[pos++];
					break;
				case 't':
					pos++;
					if (pos == format.size()) {
						throw InvalidInputException("unknown format specifier");
					}
					separator = format[pos++];
					break;
				default:
					break;
				}
			}
			if (pos < format.size() && format[pos] == '#') {
				spec += format[pos++];
			}
			if (pos < format.size() && format[pos] == '0') {
				spec += format[pos++];
			}
			if (pos < format.size()) {
				if (IsFormatDigit(format[pos])) {
					auto start = pos;
					ParseFormatInt(format, pos);
					spec += format.substr(start, pos - start);
				} else if (format[pos] == '{') {
					ParseFormatDynamicSpec(format, pos, ids, spec);
				}
			}
			if (pos < format.size() && format[pos] == '.') {
				pos++;
				spec += '.';
				if (pos < format.size() && IsFormatDigit(format[pos])) {
					auto start = pos;
					ParseFormatInt(format, pos);
					spec += format.substr(start, pos - start);
				} else if (pos < format.size() && format[pos] == '{') {
					ParseFormatDynamicSpec(format, pos, ids, spec);
				} else {
					throw InvalidInputException("missing precision specifier");
				}
			}
			if (pos < format.size() && format[pos] != '}') {
				type = format[pos++];
			}
		}
		if (pos == format.size() || format[pos] != '}') {
			throw InvalidInputException("unknown format specifier");
		}
	}
	pos++;
	auto localized = false;
	auto is_integer = IsIntegerArgument(arg) && (arg.type != FormatArgumentType::BOOLEAN || type != '\0');
	if (is_integer) {
		if (separator != '\0') {
			localized = true;
			type = '\0';
		} else {
			switch (type) {
			case '\0':
			case 'd':
			case 'x':
			case 'X':
			case 'b':
			case 'B':
			case 'o':
				break;
			case 'n':
			case 'l':
			case 'L':
				localized = true;
				type = '\0';
				break;
			default:
				throw InvalidInputException(string("Invalid type specifier \"") + type +
				                            "\" for formatting a value of type int");
			}
		}
	} else if (arg.type == FormatArgumentType::DOUBLE) {
		localized = separator != '\0';
		if (type == 'n' || type == 'l' || type == 'L') {
			localized = true;
			type = '\0';
		} else if (type != '\0' && !IsFloatPresentation(type)) {
			throw InvalidInputException(string("Invalid type specifier \"") + type +
			                            "\" for formatting a value of type float");
		}
	} else {
		separator = '\0';
		if (arg.type == FormatArgumentType::STRING && type != '\0' && type != 's') {
			throw InvalidInputException(string("Invalid type specifier \"") + type +
			                            "\" for formatting a value of type string");
		}
	}
	if (localized) {
		spec += 'L';
	}
	if (type != '\0') {
		spec += type;
	}
	string field = "{" + std::to_string(arg_index);
	if (!spec.empty()) {
		field += ':';
		field += spec;
	}
	field += '}';
	WithFmtErrors([&]() {
		fmt::vformat_to(std::back_inserter(out), GroupingLocale(localized ? separator : '\0'), fmt::string_view(field),
		                fmt_args);
	});
}

} // namespace

string StringFormat::Printf(std::string_view format, const vector<FormatArgument> &args) {
	string result;
	FormatArgumentIds ids(args.size());
	idx_t start = 0;
	idx_t pos = 0;
	while (pos < format.size()) {
		if (format[pos++] != '%') {
			continue;
		}
		if (pos < format.size() && format[pos] == '%') {
			result.append(format.substr(start, pos - start));
			start = ++pos;
			continue;
		}
		result.append(format.substr(start, pos - 1 - start));
		if (pos == format.size()) {
			throw InvalidInputException("invalid format string");
		}
		PrintfSpecs specs;
		auto arg_index = ParsePrintfHeader(format, pos, specs, args, ids);
		if (arg_index == 0) {
			throw InvalidInputException("argument index out of range");
		}
		auto empty_precision = false;
		if (pos < format.size() && format[pos] == '.') {
			pos++;
			if (pos < format.size() && IsFormatDigit(format[pos])) {
				specs.precision = ParseFormatInt(format, pos);
			} else if (pos < format.size() && format[pos] == '*') {
				pos++;
				specs.precision = PrintfPrecisionValue(args[ids.Next()]);
			} else {
				specs.precision = 0;
				empty_precision = true;
			}
		}
		auto &arg = args[arg_index < 0 ? ids.Next() : ids.Manual(static_cast<idx_t>(arg_index - 1))];
		if (specs.alt && IsIntegerArgument(arg) && IntegerArgumentValue(arg).bits == 0) {
			specs.alt = false;
		}
		if (specs.fill == '0') {
			if (arg.type == FormatArgumentType::STRING) {
				specs.fill = ' ';
			} else {
				specs.align = FormatAlign::NUMERIC;
			}
		}
		auto length = ParsePrintfLength(format, pos);
		if (pos == format.size()) {
			throw InvalidInputException("invalid format string");
		}
		specs.type = format[pos++];
		start = pos;
		AppendPrintfArgument(result, arg, length, empty_precision, specs);
	}
	result.append(format.substr(start));
	return result;
}

string StringFormat::Format(std::string_view format, const vector<FormatArgument> &args) {
	vector<fmt::basic_format_arg<fmt::format_context>> fmt_arg_values;
	fmt_arg_values.reserve(args.size());
	for (auto &arg : args) {
		fmt_arg_values.push_back(ToFmtArgument(arg));
	}
	fmt::format_args fmt_args(fmt_arg_values.data(), static_cast<int>(fmt_arg_values.size()));
	string result;
	FormatArgumentIds ids(args.size());
	idx_t pos = 0;
	while (pos < format.size()) {
		auto next = format.find_first_of("{}", pos);
		if (next == std::string_view::npos) {
			result.append(format.substr(pos));
			break;
		}
		result.append(format.substr(pos, next - pos));
		pos = next + 1;
		if (format[next] == '}') {
			if (pos == format.size() || format[pos] != '}') {
				throw InvalidInputException("unmatched '}' in format string");
			}
			result += '}';
			pos++;
			continue;
		}
		if (pos == format.size()) {
			throw InvalidInputException("invalid format string");
		}
		if (format[pos] == '{') {
			result += '{';
			pos++;
			continue;
		}
		AppendFormatField(result, format, pos, args, fmt_args, ids);
	}
	return result;
}

} // namespace duckdb
