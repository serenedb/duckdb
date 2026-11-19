//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/value_parsers.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/common/operator/double_cast_operator.hpp"
#include "duckdb/common/operator/integer_cast_operator.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"

#include <bit>

namespace duckdb {

inline constexpr uint64_t VALUE_POWERS_OF_TEN[] = {1,
                                                   10,
                                                   100,
                                                   1000,
                                                   10000,
                                                   100000,
                                                   1000000,
                                                   10000000,
                                                   100000000,
                                                   1000000000,
                                                   10000000000,
                                                   100000000000,
                                                   1000000000000,
                                                   10000000000000,
                                                   100000000000000,
                                                   1000000000000000,
                                                   10000000000000000,
                                                   100000000000000000,
                                                   1000000000000000000};

inline bool DigitsAreValid(uint64_t chunk) {
	const uint64_t high = chunk & 0xF0F0F0F0F0F0F0F0ULL;
	const uint64_t carried = ((chunk + 0x0606060606060606ULL) & 0xF0F0F0F0F0F0F0F0ULL) >> 4;
	return (high | carried) == 0x3333333333333333ULL;
}

inline uint64_t EightDigits(uint64_t chunk) {
	chunk = (chunk & 0x0F0F0F0F0F0F0F0FULL) * 2561 >> 8;
	chunk = (chunk & 0x00FF00FF00FF00FFULL) * 6553601 >> 16;
	return (chunk & 0x0000FFFF0000FFFFULL) * 42949672960001ULL >> 32;
}

[[gnu::always_inline]] inline bool SwarDigits(const char *value, idx_t count, uint64_t &out) {
	uint64_t chunk;
	memcpy(&chunk, value, sizeof(chunk));
	if (count < 8) {
		chunk = (chunk << ((8 - count) * 8)) | (0x3030303030303030ULL >> (count * 8));
	}
	if (!DigitsAreValid(chunk)) {
		return false;
	}
	out = EightDigits(chunk);
	return true;
}

inline bool ScalarDigits(const char *value, idx_t count, uint64_t &out) {
	uint64_t result = 0;
	for (idx_t pos = 0; pos < count; pos++) {
		const auto digit = static_cast<uint8_t>(value[pos] - '0');
		if (digit > 9) {
			return false;
		}
		result = result * 10 + digit;
	}
	out = result;
	return true;
}

[[gnu::always_inline]] inline idx_t FindByte(const char *value, idx_t size, const char *limit, char byte) {
	const uint64_t pattern = 0x0101010101010101ULL * static_cast<uint8_t>(byte);
	for (idx_t pos = 0; pos < size; pos += 8) {
		if (value + pos + 8 > limit) {
			for (; pos < size; pos++) {
				if (value[pos] == byte) {
					return pos;
				}
			}
			return size;
		}
		uint64_t chunk;
		memcpy(&chunk, value + pos, sizeof(chunk));
		const uint64_t matched = chunk ^ pattern;
		const uint64_t found = (matched - 0x0101010101010101ULL) & ~matched & 0x8080808080808080ULL;
		if (found) {
			return MinValue<idx_t>(pos + (std::countr_zero(found) >> 3), size);
		}
	}
	return size;
}

[[gnu::always_inline]] inline bool ParseDigits(const char *value, idx_t count, const char *limit, uint64_t &out) {
	if (count == 0 || count > 16) {
		return false;
	}
	if (count <= 8) {
		return value + 8 <= limit ? SwarDigits(value, count, out) : ScalarDigits(value, count, out);
	}
	uint64_t high;
	uint64_t low;
	if (!SwarDigits(value, count - 8, high) || !SwarDigits(value + count - 8, 8, low)) {
		return false;
	}
	out = high * 100000000ULL + low;
	return true;
}

template <class T>
[[gnu::always_inline]] inline bool FastInteger(const char *value, idx_t size, const char *limit, T &out) {
	if (size == 0) {
		return false;
	}
	idx_t pos = 0;
	bool negative = false;
	if (std::is_signed<T>::value && value[0] == '-') {
		negative = true;
		pos = 1;
	}
	uint64_t result;
	if (!ParseDigits(value + pos, size - pos, limit, result)) {
		return false;
	}
	if (negative) {
		if (result > static_cast<uint64_t>(NumericLimits<T>::Maximum()) + 1) {
			return false;
		}
		out = static_cast<T>(-static_cast<int64_t>(result));
		return true;
	}
	if (result > static_cast<uint64_t>(NumericLimits<T>::Maximum())) {
		return false;
	}
	out = static_cast<T>(result);
	return true;
}

template <class T>
[[gnu::always_inline]] inline bool FastDecimal(const char *value, idx_t size, const char *limit, T &out, char separator,
                                               uint8_t width, uint8_t scale) {
	if (size == 0 || width > 18) {
		return false;
	}
	idx_t pos = 0;
	bool negative = false;
	if (value[0] == '-') {
		negative = true;
		pos = 1;
	}
	const char *digits = value + pos;
	const idx_t remaining = size - pos;
	const idx_t integer_digits = FindByte(digits, remaining, limit, separator);
	const bool dot = integer_digits < remaining;
	const idx_t fraction_digits = dot ? remaining - integer_digits - 1 : 0;
	if (integer_digits == 0 || integer_digits > static_cast<idx_t>(width - scale) || fraction_digits > scale ||
	    (dot && fraction_digits == 0)) {
		return false;
	}
	uint64_t integer;
	if (!ParseDigits(digits, integer_digits, limit, integer)) {
		return false;
	}
	uint64_t fraction = 0;
	if (fraction_digits && !ParseDigits(digits + integer_digits + 1, fraction_digits, limit, fraction)) {
		return false;
	}
	const auto result = static_cast<int64_t>(integer * VALUE_POWERS_OF_TEN[scale] +
	                                         fraction * VALUE_POWERS_OF_TEN[scale - fraction_digits]);
	out = static_cast<T>(negative ? -result : result);
	return true;
}

[[gnu::always_inline]] inline bool FastDate(const char *value, idx_t size, date_t &out) {
	if (size != 10) {
		return false;
	}
	uint64_t head;
	uint64_t tail;
	memcpy(&head, value, sizeof(head));
	memcpy(&tail, value + 2, sizeof(tail));
	if (((head >> 32) & 0xFF) != '-' || (head >> 56) != '-') {
		return false;
	}
	const uint64_t digits =
	    (head & 0x00000000FFFFFFFFULL) | ((head >> 8) & 0x0000FFFF00000000ULL) | (tail & 0xFFFF000000000000ULL);
	if (!DigitsAreValid(digits)) {
		return false;
	}
	const auto packed = static_cast<int32_t>(EightDigits(digits));
	int32_t year = packed / 10000;
	const auto month = static_cast<uint32_t>(packed / 100 % 100);
	const auto day = static_cast<uint32_t>(packed % 100);
	if (year == 0 || month == 0 || month > 12 || day == 0) {
		return false;
	}
	const bool leap = (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
	static constexpr uint8_t DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (day > DAYS[month - 1] + (month == 2 && leap)) {
		return false;
	}
	year -= month <= 2;
	const int32_t era = year / 400;
	const auto year_of_era = static_cast<uint32_t>(year - era * 400);
	const uint32_t day_of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
	const uint32_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
	out = date_t(era * 146097 + static_cast<int32_t>(day_of_era) - 719468);
	return true;
}

template <class T>
struct ParsedAs {
	using type = T;
};

template <class T>
struct SignedParser {
	bool operator()(const char *value, idx_t size, T &out) const {
		return (!strict && FastInteger<T>(value, size, limit, out)) ||
		       TrySimpleIntegerCast<T>(value, size, out, strict);
	}
	const char *limit;
	bool strict;
};

template <class T>
struct UnsignedParser {
	bool operator()(const char *value, idx_t size, T &out) const {
		return (!strict && FastInteger<T>(value, size, limit, out)) ||
		       TrySimpleIntegerCast<T, false>(value, size, out, strict);
	}
	const char *limit;
	bool strict;
};

template <class T>
struct DecimalParser {
	bool operator()(const char *value, idx_t size, T &out) const {
		if (!strict && FastDecimal<T>(value, size, limit, out, separator, width, scale)) {
			return true;
		}
		if (separator == ',') {
			return TryDecimalStringCast<T, ','>(value, size, out, width, scale);
		}
		return TryDecimalStringCast<T>(value, size, out, width, scale);
	}
	const char *limit;
	char separator;
	uint8_t width;
	uint8_t scale;
	bool strict;
};

struct ValueParseContext {
	const StrpTimeFormat *date_format;
	const StrpTimeFormat *timestamp_format;
	const char *limit;
	char separator;
	bool strict = false;
};

template <class FN>
bool WithValueParser(const LogicalType &type, const ValueParseContext &context, FN &&fn) {
	const char separator = context.separator;
	const char *limit = context.limit;
	const bool strict = context.strict;
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return fn(ParsedAs<bool>(), [strict](const char *value, idx_t size, bool &out) {
			return TryCastStringBool(value, size, out, strict);
		});
	case LogicalTypeId::TINYINT:
		return fn(ParsedAs<int8_t>(), SignedParser<int8_t> {limit, strict});
	case LogicalTypeId::SMALLINT:
		return fn(ParsedAs<int16_t>(), SignedParser<int16_t> {limit, strict});
	case LogicalTypeId::INTEGER:
		return fn(ParsedAs<int32_t>(), SignedParser<int32_t> {limit, strict});
	case LogicalTypeId::BIGINT:
		return fn(ParsedAs<int64_t>(), SignedParser<int64_t> {limit, strict});
	case LogicalTypeId::UTINYINT:
		return fn(ParsedAs<uint8_t>(), UnsignedParser<uint8_t> {limit, strict});
	case LogicalTypeId::USMALLINT:
		return fn(ParsedAs<uint16_t>(), UnsignedParser<uint16_t> {limit, strict});
	case LogicalTypeId::UINTEGER:
		return fn(ParsedAs<uint32_t>(), UnsignedParser<uint32_t> {limit, strict});
	case LogicalTypeId::UBIGINT:
		return fn(ParsedAs<uint64_t>(), UnsignedParser<uint64_t> {limit, strict});
	case LogicalTypeId::HUGEINT:
		return fn(ParsedAs<hugeint_t>(), [strict](const char *value, idx_t size, hugeint_t &out) {
			return TryCast::Operation<string_t, hugeint_t>(string_t(value, NumericCast<uint32_t>(size)), out, strict);
		});
	case LogicalTypeId::UHUGEINT:
		return fn(ParsedAs<uhugeint_t>(), [strict](const char *value, idx_t size, uhugeint_t &out) {
			return TryCast::Operation<string_t, uhugeint_t>(string_t(value, NumericCast<uint32_t>(size)), out, strict);
		});
	case LogicalTypeId::DOUBLE:
		return fn(ParsedAs<double>(), [separator, strict](const char *value, idx_t size, double &out) {
			return TryDoubleCast<double>(value, size, out, strict, separator);
		});
	case LogicalTypeId::FLOAT:
		return fn(ParsedAs<float>(), [separator, strict](const char *value, idx_t size, float &out) {
			return TryDoubleCast<float>(value, size, out, strict, separator);
		});
	case LogicalTypeId::DATE: {
		const auto format = context.date_format;
		if (!format->Empty()) {
			return fn(ParsedAs<date_t>(), [format](const char *value, idx_t size, date_t &out) {
				return format->TryParseDate(value, size, out);
			});
		}
		return fn(ParsedAs<date_t>(), [strict](const char *value, idx_t size, date_t &out) {
			if (FastDate(value, size, out)) {
				return true;
			}
			idx_t pos;
			bool special;
			return Date::TryConvertDate(value, size, pos, out, special, strict) == DateCastResult::SUCCESS;
		});
	}
	case LogicalTypeId::TIME:
		return fn(ParsedAs<dtime_t>(), [strict](const char *value, idx_t size, dtime_t &out) {
			idx_t pos;
			return Time::TryConvertTime(value, size, pos, out, strict);
		});
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ: {
		const bool tz = type.id() == LogicalTypeId::TIMESTAMP_TZ;
		const auto format = context.timestamp_format;
		if (!format->Empty()) {
			return fn(ParsedAs<timestamp_t>(), [format](const char *value, idx_t size, timestamp_t &out) {
				return format->TryParseTimestamp(value, size, out);
			});
		}
		return fn(ParsedAs<timestamp_t>(), [tz, strict](const char *value, idx_t size, timestamp_t &out) {
			return Timestamp::TryConvertTimestamp(value, size, out, tz, nullptr, strict) ==
			       TimestampCastResult::SUCCESS;
		});
	}
	case LogicalTypeId::DECIMAL: {
		uint8_t width;
		uint8_t scale;
		type.GetDecimalProperties(width, scale);
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			return fn(ParsedAs<int16_t>(), DecimalParser<int16_t> {limit, separator, width, scale, strict});
		case PhysicalType::INT32:
			return fn(ParsedAs<int32_t>(), DecimalParser<int32_t> {limit, separator, width, scale, strict});
		case PhysicalType::INT64:
			return fn(ParsedAs<int64_t>(), DecimalParser<int64_t> {limit, separator, width, scale, strict});
		case PhysicalType::INT128:
			return fn(ParsedAs<hugeint_t>(), DecimalParser<hugeint_t> {limit, separator, width, scale, strict});
		default:
			throw InternalException("Invalid physical type for a decimal");
		}
	}
	default:
		throw InternalException("No value parser for %s", type.ToString());
	}
}

bool HasValueParser(const LogicalType &type);
bool ValueParses(const LogicalType &type, const char *value, idx_t size, const ValueParseContext &context);

} // namespace duckdb
