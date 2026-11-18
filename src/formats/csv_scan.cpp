#include "duckdb/formats/csv_scan.hpp"

#include "absl/cleanup/cleanup.h"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/common/operator/double_cast_operator.hpp"
#include "duckdb/common/operator/integer_cast_operator.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parallel/callback_async_task.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "simdutf.h"
#include "utf8proc_wrapper.hpp"

namespace duckdb {

namespace {

constexpr idx_t STITCH_BYTES = 1 << 16;
constexpr idx_t LOOKUP_BYTES = 1 << 13;
constexpr idx_t RANGE_BYTES = 1 << 19;
constexpr idx_t RANGE_OVERLAP = 1 << 12;
constexpr idx_t LEAN_RANGE_MIN = 1 << 20;
constexpr idx_t LEAN_RANGE_MAX = 1 << 23;
constexpr idx_t LEAN_PIECE = 1 << 16;
constexpr idx_t LEAN_WINDOW = 1 << 20;
constexpr idx_t LEAN_UNSETTLED = 1 << 22;

bool LeanDecodes(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::VARCHAR:
		return true;
	default:
		return false;
	}
}

constexpr uint64_t POWERS_OF_TEN[] = {1,
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
	const auto result =
	    static_cast<int64_t>(integer * POWERS_OF_TEN[scale] + fraction * POWERS_OF_TEN[scale - fraction_digits]);
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

[[gnu::always_inline]] inline void MakeString(const char *value, idx_t size, const char *limit, string_t &out) {
	if constexpr (string_t::INLINE_LENGTH == 12 && string_t::PREFIX_LENGTH == 4) {
		uint64_t words[2];
		if (size <= 12 && value + 12 <= limit) {
			uint32_t head;
			uint64_t tail;
			memcpy(&head, value, sizeof(head));
			memcpy(&tail, value + 4, sizeof(tail));
			const idx_t head_bytes = MinValue<idx_t>(size, 4);
			const idx_t tail_bytes = size > 4 ? size - 4 : 0;
			head &= head_bytes == 4 ? ~uint32_t(0) : (uint32_t(1) << (head_bytes * 8)) - 1;
			tail &= tail_bytes == 8 ? ~uint64_t(0) : (uint64_t(1) << (tail_bytes * 8)) - 1;
			words[0] = size | (uint64_t(head) << 32);
			words[1] = tail;
			memcpy(&out, words, sizeof(words));
			return;
		}
		if (size > 12) {
			uint32_t prefix;
			memcpy(&prefix, value, sizeof(prefix));
			words[0] = size | (uint64_t(prefix) << 32);
			words[1] = reinterpret_cast<uintptr_t>(value);
			memcpy(&out, words, sizeof(words));
			return;
		}
	}
	out = string_t(value, UnsafeNumericCast<uint32_t>(size));
}

inline bool LeanUnquote(const char *data, char quote, bool spaced, idx_t &begin, idx_t &end) {
	idx_t open = begin;
	if (spaced && end - open > 1 && data[open] == ' ' && data[open + 1] == quote) {
		open++;
	}
	if (end <= open || data[open] != quote) {
		return false;
	}
	idx_t close = end;
	while (spaced && close > open + 1 && data[close - 1] == ' ') {
		close--;
	}
	begin = open + 1;
	end = close - 1;
	return true;
}

template <class T, class PARSE>
bool LeanValues(const LeanSource &source, idx_t count, bool force_not_null, Vector &vector, PARSE parse) {
	T *const out = FlatVector::GetDataMutable<T>(vector);
	const uint32_t *const begins = source.begins;
	const idx_t begin_stride = source.begin_stride;
	const idx_t begin_bias = source.begin_bias;
	const uint32_t *const ends = source.ends;
	const idx_t stride = source.stride;
	const char *const data = source.data;
	const idx_t shift = source.shift;
	const char quote = source.quote;
	const bool quotes = source.quotes;
	const bool doubled = source.doubled;
	const uint32_t flag_test = quotes || source.padded ? ROW_FIELD_FLAG : 0;
	const bool empty_only_null = source.empty_only_null && source.allow_quoted_nulls;
	const auto selected = source.selected;
	const idx_t first = source.first;
	for (idx_t row = first; row < first + count; row++) {
		if (selected && !(*selected)[row]) {
			FlatVector::SetNull(vector, row, true);
			continue;
		}
		const uint32_t end_bits = ends[row * stride];
		idx_t begin = (begins[row * begin_stride] & ROW_FIELD_MASK) + begin_bias + shift;
		idx_t end = (end_bits & ROW_FIELD_MASK) + shift;
		bool quoted = false;
		if (end_bits & flag_test) {
			if (begin > end) {
				FlatVector::SetNull(vector, row, true);
				continue;
			}
			quoted = quotes && LeanUnquote(data, quote, source.spaced, begin, end);
		}
		const char *value = data + begin;
		const idx_t size = end - begin;
		if (empty_only_null ? size == 0 : source.IsNull(value, size, quoted)) {
			if (force_not_null) {
				return false;
			}
			FlatVector::SetNull(vector, row, true);
			continue;
		}
		if (quoted && doubled && memchr(value, source.escape_probe, size)) {
			return false;
		}
		if (!parse(value, size, out[row])) {
			return false;
		}
	}
	return true;
}

bool IsStringColumnRef(const Expression &expr) {
	return ExpressionFilter::IsSimpleFilterColumnRef(expr) && expr.GetReturnType().id() == LogicalTypeId::VARCHAR &&
	       !expr.GetReturnType().IsJSONType();
}

const string *StringConstant(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return nullptr;
	}
	auto &value = expr.Cast<BoundConstantExpression>().GetValue();
	if (value.IsNull() || value.type().id() != LogicalTypeId::VARCHAR) {
		return nullptr;
	}
	return &StringValue::Get(value);
}

bool CollectRawPattern(const Expression &expr, bool negated, vector<CsvRawPredicate> &out) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &function = expr.Cast<BoundFunctionExpression>();
	auto &children = function.GetChildren();
	if (children.size() != 2 || !IsStringColumnRef(*children[0])) {
		return false;
	}
	auto pattern = StringConstant(*children[1]);
	if (!pattern) {
		return false;
	}
	auto &name = function.Function().GetName();
	CsvRawPredicate predicate {negated ? CsvRawKind::NOT_LIKE : CsvRawKind::LIKE};
	if (name == "prefix" || name == "starts_with" || name == "^@") {
		predicate.anchored_start = true;
		predicate.values.push_back(*pattern);
	} else if (name == "suffix" || name == "ends_with") {
		predicate.anchored_end = true;
		predicate.values.push_back(*pattern);
	} else if (name == "contains") {
		predicate.values.push_back(*pattern);
	} else if (name == "~~" || name == "!~~") {
		if (pattern->find_first_of("_\\") != string::npos) {
			return false;
		}
		if (name == "!~~") {
			predicate.kind = negated ? CsvRawKind::LIKE : CsvRawKind::NOT_LIKE;
		}
		predicate.anchored_start = pattern->empty() || pattern->front() != '%';
		predicate.anchored_end = pattern->empty() || pattern->back() != '%';
		idx_t begin = 0;
		while (begin <= pattern->size()) {
			auto end = std::min(pattern->find('%', begin), pattern->size());
			if (end > begin) {
				predicate.values.push_back(pattern->substr(begin, end - begin));
			}
			begin = end + 1;
		}
	} else {
		return false;
	}
	out.push_back(std::move(predicate));
	return true;
}

bool CollectRawPredicates(const Expression &expr, bool string_column, vector<CsvRawPredicate> &out) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		if (expr.GetExpressionType() != ExpressionType::CONJUNCTION_AND) {
			return false;
		}
		for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
			if (!CollectRawPredicates(*child, string_column, out)) {
				return false;
			}
		}
		return true;
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op = expr.Cast<BoundOperatorExpression>();
		const auto type = op.GetExpressionType();
		auto &children = op.GetChildren();
		if (type == ExpressionType::OPERATOR_NOT) {
			return string_column && children.size() == 1 && CollectRawPattern(*children[0], true, out);
		}
		if (children.empty() || !ExpressionFilter::IsSimpleFilterColumnRef(*children[0])) {
			return false;
		}
		if (type == ExpressionType::OPERATOR_IS_NULL || type == ExpressionType::OPERATOR_IS_NOT_NULL) {
			out.push_back({type == ExpressionType::OPERATOR_IS_NULL ? CsvRawKind::IS_NULL : CsvRawKind::IS_NOT_NULL});
			return true;
		}
		if (type != ExpressionType::COMPARE_IN || !string_column) {
			return false;
		}
		CsvRawPredicate predicate {CsvRawKind::IN_LIST};
		for (idx_t i = 1; i < children.size(); i++) {
			auto value = StringConstant(*children[i]);
			if (!value) {
				return false;
			}
			predicate.values.push_back(*value);
		}
		std::sort(predicate.values.begin(), predicate.values.end());
		out.push_back(std::move(predicate));
		return true;
	}
	case ExpressionClass::BOUND_FUNCTION: {
		if (!string_column) {
			return false;
		}
		auto &function = expr.Cast<BoundFunctionExpression>();
		auto type = function.GetExpressionType();
		if (type == ExpressionType::BOUND_FUNCTION) {
			return CollectRawPattern(function, false, out);
		}
		if (!BoundComparisonExpression::IsComparison(type) || type == ExpressionType::COMPARE_DISTINCT_FROM ||
		    type == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			return false;
		}
		auto constant = ExpressionFilter::TryGetColumnConstantComparison(function, type);
		if (!constant) {
			return false;
		}
		auto &value = constant->GetValue();
		if (value.IsNull() || value.type().id() != LogicalTypeId::VARCHAR) {
			return false;
		}
		out.push_back({CsvRawKind::COMPARE, type, {StringValue::Get(value)}});
		return true;
	}
	default:
		return false;
	}
}

bool RawLike(const CsvRawPredicate &predicate, std::string_view field) {
	auto &segments = predicate.values;
	idx_t first = 0;
	idx_t last = segments.size();
	idx_t begin = 0;
	idx_t end = field.size();
	if (predicate.anchored_start && last > 0) {
		if (!field.starts_with(segments[0])) {
			return false;
		}
		begin = segments[0].size();
		first = 1;
	}
	if (predicate.anchored_end) {
		if (first == last) {
			return begin == end;
		}
		auto &segment = segments[last - 1];
		if (end - begin < segment.size() || !field.ends_with(segment)) {
			return false;
		}
		end -= segment.size();
		last--;
	}
	const auto window = field.substr(0, end);
	for (idx_t i = first; i < last; i++) {
		const auto position = window.find(segments[i], begin);
		if (position == std::string_view::npos) {
			return false;
		}
		begin = position + segments[i].size();
	}
	return true;
}

bool RawCompare(ExpressionType type, std::string_view field, const string &constant) {
	const string_t left(field.data(), UnsafeNumericCast<uint32_t>(field.size()));
	const string_t right(constant.data(), UnsafeNumericCast<uint32_t>(constant.size()));
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		return Equals::Operation(left, right);
	case ExpressionType::COMPARE_NOTEQUAL:
		return NotEquals::Operation(left, right);
	case ExpressionType::COMPARE_LESSTHAN:
		return LessThan::Operation(left, right);
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return LessThanEquals::Operation(left, right);
	case ExpressionType::COMPARE_GREATERTHAN:
		return GreaterThan::Operation(left, right);
	default:
		return GreaterThanEquals::Operation(left, right);
	}
}

bool RawPasses(const vector<CsvRawPredicate> &predicates, const char *value, idx_t size, bool null) {
	for (auto &predicate : predicates) {
		if (predicate.kind == CsvRawKind::IS_NULL) {
			if (!null) {
				return false;
			}
			continue;
		}
		if (null) {
			return false;
		}
		const std::string_view field(value, size);
		switch (predicate.kind) {
		case CsvRawKind::IS_NOT_NULL:
			break;
		case CsvRawKind::IN_LIST:
			if (!std::binary_search(predicate.values.begin(), predicate.values.end(), field)) {
				return false;
			}
			break;
		case CsvRawKind::LIKE:
		case CsvRawKind::NOT_LIKE:
			if (RawLike(predicate, field) != (predicate.kind == CsvRawKind::LIKE)) {
				return false;
			}
			break;
		default:
			if (!RawCompare(predicate.compare, field, predicate.values[0])) {
				return false;
			}
			break;
		}
	}
	return true;
}

void LeanRawPredicates(const LeanSource &source, idx_t count, const vector<CsvRawPredicate> &predicates,
                       vector<bool> &selected) {
	const uint32_t flag_test = source.quotes || source.padded ? ROW_FIELD_FLAG : 0;
	const bool empty_only_null = source.empty_only_null && source.allow_quoted_nulls;
	for (idx_t row = 0; row < count; row++) {
		if (!selected[row]) {
			continue;
		}
		const uint32_t end_bits = source.ends[row * source.stride];
		idx_t begin = (source.begins[row * source.begin_stride] & ROW_FIELD_MASK) + source.begin_bias + source.shift;
		idx_t end = (end_bits & ROW_FIELD_MASK) + source.shift;
		bool quoted = false;
		if (end_bits & flag_test) {
			if (begin > end) {
				selected[row] = RawPasses(predicates, nullptr, 0, true);
				continue;
			}
			quoted = source.quotes && LeanUnquote(source.data, source.quote, source.spaced, begin, end);
		}
		const char *value = source.data + begin;
		const idx_t size = end - begin;
		if (quoted && source.doubled && memchr(value, source.escape_probe, size)) {
			continue;
		}
		const bool null = empty_only_null ? size == 0 : source.IsNull(value, size, quoted);
		selected[row] = RawPasses(predicates, value, size, null);
	}
}

bool LeanStrings(const LeanSource &source, idx_t count, const CsvScanColumn &column, bool validate,
                 const CsvGrammar &grammar, Vector &vector) {
	string_t *const out = FlatVector::GetDataMutable<string_t>(vector);
	const uint32_t *const begins = source.begins;
	const idx_t begin_stride = source.begin_stride;
	const idx_t begin_bias = source.begin_bias;
	const uint32_t *const ends = source.ends;
	const idx_t stride = source.stride;
	const char *const limit = source.limit;
	const char *const data = source.data;
	const idx_t shift = source.shift;
	const char quote = source.quote;
	const bool quotes = source.quotes;
	const uint32_t flag_test = quotes || source.padded ? ROW_FIELD_FLAG : 0;
	const bool empty_only_null = source.empty_only_null && source.allow_quoted_nulls;
	const auto selected = source.selected;
	const idx_t first = source.first;
	for (idx_t row = first; row < first + count; row++) {
		if (selected && !(*selected)[row]) {
			FlatVector::SetNull(vector, row, true);
			continue;
		}
		const uint32_t end_bits = ends[row * stride];
		idx_t begin = (begins[row * begin_stride] & ROW_FIELD_MASK) + begin_bias + shift;
		idx_t end = (end_bits & ROW_FIELD_MASK) + shift;
		bool quoted = false;
		if (end_bits & flag_test) {
			if (begin > end) {
				if (source.pad_empty) {
					out[row] = string_t();
				} else {
					FlatVector::SetNull(vector, row, true);
				}
				continue;
			}
			quoted = quotes && LeanUnquote(data, quote, source.spaced, begin, end);
		}
		const char *value = data + begin;
		const idx_t size = end - begin;
		if (empty_only_null ? size == 0 : source.IsNull(value, size, quoted)) {
			if (column.force_not_null) {
				out[row] = string_t();
			} else {
				FlatVector::SetNull(vector, row, true);
			}
			continue;
		}
		if (validate && !Utf8Proc::IsValid(value, UnsafeNumericCast<uint32_t>(size))) {
			return false;
		}
		if (quoted && source.doubled && memchr(value, source.escape_probe, size)) {
			out[row] = CsvRemoveEscape(value, size, grammar.escape, grammar.quote, grammar.strict_mode, vector);
			continue;
		}
		MakeString(value, size, limit, out[row]);
	}
	return true;
}

template <class T>
struct ParsedAs {
	using type = T;
};

template <class T>
struct SignedParser {
	bool operator()(const char *value, idx_t size, T &out) const {
		return FastInteger<T>(value, size, limit, out) || TrySimpleIntegerCast<T>(value, size, out, false);
	}
	const char *limit;
};

template <class T>
struct UnsignedParser {
	bool operator()(const char *value, idx_t size, T &out) const {
		return FastInteger<T>(value, size, limit, out) || TrySimpleIntegerCast<T, false>(value, size, out, false);
	}
	const char *limit;
};

template <class T>
struct DecimalParser {
	bool operator()(const char *value, idx_t size, T &out) const {
		if (FastDecimal<T>(value, size, limit, out, separator, width, scale)) {
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
};

struct ParseContext {
	const StrpTimeFormat *date_format;
	const StrpTimeFormat *timestamp_format;
	const char *limit;
	char separator;
};

template <class FN>
bool WithParser(const LogicalType &type, const ParseContext &context, FN &&fn) {
	const char separator = context.separator;
	const char *limit = context.limit;
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return fn(ParsedAs<bool>(),
		          [](const char *value, idx_t size, bool &out) { return TryCastStringBool(value, size, out, false); });
	case LogicalTypeId::TINYINT:
		return fn(ParsedAs<int8_t>(), SignedParser<int8_t> {limit});
	case LogicalTypeId::SMALLINT:
		return fn(ParsedAs<int16_t>(), SignedParser<int16_t> {limit});
	case LogicalTypeId::INTEGER:
		return fn(ParsedAs<int32_t>(), SignedParser<int32_t> {limit});
	case LogicalTypeId::BIGINT:
		return fn(ParsedAs<int64_t>(), SignedParser<int64_t> {limit});
	case LogicalTypeId::UTINYINT:
		return fn(ParsedAs<uint8_t>(), UnsignedParser<uint8_t> {limit});
	case LogicalTypeId::USMALLINT:
		return fn(ParsedAs<uint16_t>(), UnsignedParser<uint16_t> {limit});
	case LogicalTypeId::UINTEGER:
		return fn(ParsedAs<uint32_t>(), UnsignedParser<uint32_t> {limit});
	case LogicalTypeId::UBIGINT:
		return fn(ParsedAs<uint64_t>(), UnsignedParser<uint64_t> {limit});
	case LogicalTypeId::HUGEINT:
		return fn(ParsedAs<hugeint_t>(), [](const char *value, idx_t size, hugeint_t &out) {
			return TryCast::Operation<string_t, hugeint_t>(string_t(value, NumericCast<uint32_t>(size)), out, false);
		});
	case LogicalTypeId::UHUGEINT:
		return fn(ParsedAs<uhugeint_t>(), [](const char *value, idx_t size, uhugeint_t &out) {
			return TryCast::Operation<string_t, uhugeint_t>(string_t(value, NumericCast<uint32_t>(size)), out, false);
		});
	case LogicalTypeId::DOUBLE:
		return fn(ParsedAs<double>(), [separator](const char *value, idx_t size, double &out) {
			return TryDoubleCast<double>(value, size, out, false, separator);
		});
	case LogicalTypeId::FLOAT:
		return fn(ParsedAs<float>(), [separator](const char *value, idx_t size, float &out) {
			return TryDoubleCast<float>(value, size, out, false, separator);
		});
	case LogicalTypeId::DATE: {
		const auto format = context.date_format;
		if (!format->Empty()) {
			return fn(ParsedAs<date_t>(), [format](const char *value, idx_t size, date_t &out) {
				return format->TryParseDate(value, size, out);
			});
		}
		return fn(ParsedAs<date_t>(), [](const char *value, idx_t size, date_t &out) {
			if (FastDate(value, size, out)) {
				return true;
			}
			idx_t pos;
			bool special;
			return Date::TryConvertDate(value, size, pos, out, special, false) == DateCastResult::SUCCESS;
		});
	}
	case LogicalTypeId::TIME:
		return fn(ParsedAs<dtime_t>(), [](const char *value, idx_t size, dtime_t &out) {
			idx_t pos;
			return Time::TryConvertTime(value, size, pos, out, false);
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
		return fn(ParsedAs<timestamp_t>(), [tz](const char *value, idx_t size, timestamp_t &out) {
			return Timestamp::TryConvertTimestamp(value, size, out, tz) == TimestampCastResult::SUCCESS;
		});
	}
	case LogicalTypeId::DECIMAL: {
		uint8_t width;
		uint8_t scale;
		type.GetDecimalProperties(width, scale);
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			return fn(ParsedAs<int16_t>(), DecimalParser<int16_t> {limit, separator, width, scale});
		case PhysicalType::INT32:
			return fn(ParsedAs<int32_t>(), DecimalParser<int32_t> {limit, separator, width, scale});
		case PhysicalType::INT64:
			return fn(ParsedAs<int64_t>(), DecimalParser<int64_t> {limit, separator, width, scale});
		case PhysicalType::INT128:
			return fn(ParsedAs<hugeint_t>(), DecimalParser<hugeint_t> {limit, separator, width, scale});
		default:
			throw InternalException("Invalid physical type for a decimal");
		}
	}
	default:
		throw InternalException("No CSV value parser for %s", type.ToString());
	}
}

string CastMessage(const char *value, idx_t size, const string &type) {
	string message = "Could not convert string \"" + string(value, size) + "\" to '" + type + "'";
	CsvSanitize(message);
	return message;
}

idx_t SkipBom(const char *data, idx_t size) {
	if (size >= 3 && data[0] == '\xEF' && data[1] == '\xBB' && data[2] == '\xBF') {
		return 3;
	}
	return 0;
}

} // namespace

bool CsvDecodesDirectly(const LogicalType &type, bool icu_loaded) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::BIGNUM:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIME:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::BOOLEAN:
		return true;
	case LogicalTypeId::TIMESTAMP_TZ:
		return !icu_loaded;
	case LogicalTypeId::VARCHAR:
		return !type.IsJSONType();
	default:
		return false;
	}
}

string_t CsvRemoveEscape(const char *value, idx_t size, char escape, char quote, bool strict_mode, Vector &vector) {
	auto keep = [&](idx_t pos, bool &escaped) {
		const char c = value[pos];
		if (c == escape && !escaped) {
			escaped = true;
			return false;
		}
		const bool kept = c != quote || escaped || !strict_mode;
		escaped = false;
		return kept;
	};
	idx_t length = 0;
	bool escaped = false;
	for (idx_t pos = 0; pos < size; pos++) {
		length += keep(pos, escaped);
	}
	auto result = StringVector::EmptyString(vector, length);
	auto out = result.GetDataWriteable();
	escaped = false;
	for (idx_t pos = 0, written = 0; pos < size; pos++) {
		if (keep(pos, escaped)) {
			out[written++] = value[pos];
		}
	}
	result.Finalize();
	return result;
}

void CsvSanitize(string &text) {
	text.push_back('\0');
	Utf8Proc::MakeValid(&text[0], text.size());
	text.pop_back();
}

static CsvScanSource FileSource(CSVFileScan &file) {
	CsvScanSource source;
	source.state_machine = file.state_machine;
	source.buffer_manager = file.buffer_manager;
	source.error_handler = file.error_handler;
	source.projection.assign(file.projection_ids.begin(), file.projection_ids.end());
	source.types = file.file_types;
	source.names = file.GetNames();
	source.path = file.GetFileName();
	source.file_row_number = file.file_row_number_idx;
	source.bytes_read = &file.bytes_read;
	source.placeholder = file.column_ids.empty();
	return source;
}

static CsvScanSource FileSource(CSVFileScan &file, optional_ptr<TableFilterSet> filters) {
	auto source = FileSource(file);
	source.filters = filters;
	return source;
}

CsvScanGlobal::CsvScanGlobal(ClientContext &context_p, CSVFileScan &file, optional_ptr<TableFilterSet> filters_p)
    : CsvScanGlobal(context_p, file.options, FileSource(file, filters_p)) {
}

CsvScanGlobal::CsvScanGlobal(ClientContext &context_p, const CSVReaderOptions &options_p, CsvScanSource source_p)
    : context(context_p), options(options_p), source(std::move(source_p)) {
	auto &state_machine = *source.state_machine;
	grammar = CsvGrammar::For(state_machine);

	const bool icu_loaded = context.db->ExtensionIsLoaded("icu");
	for (idx_t i = 0; i < source.projection.size(); i++) {
		CsvScanColumn column;
		column.file_column = source.projection[i].first;
		column.output_column = source.projection[i].second;
		column.type = source.types[i];
		auto parse_type = column.type.IsJSONType() ? LogicalType::VARCHAR : column.type;
		if (CsvDecodesDirectly(parse_type, icu_loaded)) {
			column.parse_type = parse_type;
			column.validate_utf8 = true;
		} else {
			column.parse_type = LogicalType::VARCHAR;
			column.validate_utf8 = column.type.id() == LogicalTypeId::VARCHAR || column.type.IsNested();
		}
		column.force_not_null =
		    column.file_column < options.force_not_null.size() && options.force_not_null[column.file_column];
		columns.push_back(std::move(column));
	}
	filtered.assign(columns.size(), false);
	if (source.filters) {
		for (auto &entry : *source.filters) {
			CsvScanFilter filter {entry.GetIndex(), entry.Filter()};
			for (idx_t i = 0; i < columns.size(); i++) {
				if (columns[i].output_column == filter.output_column) {
					filtered[i] = true;
					filter.column = i;
				}
			}
			auto &expression_filter = ExpressionFilter::GetExpressionFilter(entry.Filter(), "read_csv");
			if (filter.column != DConstants::INVALID_INDEX && expression_filter.column_indexes.empty() &&
			    !columns[filter.column].force_not_null) {
				auto &column = columns[filter.column];
				const bool string_column = column.type.id() == LogicalTypeId::VARCHAR && !column.type.IsJSONType() &&
				                           column.parse_type.id() == LogicalTypeId::VARCHAR;
				if (!CollectRawPredicates(*expression_filter.expr, string_column, filter.raw)) {
					filter.raw.clear();
				}
			}
			filters.push_back(std::move(filter));
		}
	}
	late_materialization = !filters.empty() && !options.store_rejects.GetValue();
	date_format = options.dialect_options.date_format.at(LogicalTypeId::DATE).GetValue();
	timestamp_format = options.dialect_options.date_format.at(LogicalTypeId::TIMESTAMP).GetValue();
	decimal_separator = options.decimal_separator[0];
	null_strings = options.null_str;
	for (auto &null_string : null_strings) {
		empty_is_null = empty_is_null || null_string.empty();
	}
	ignore_errors = options.IgnoreErrors();
	print_line = options.store_rejects.GetValue() || !options.ignore_errors.GetValue();
	if (source.sniffing) {
		skip_rows = state_machine.dialect_options.skip_rows.GetValue();
		skip_bom = skip_rows + state_machine.dialect_options.header.GetValue() == 0;
	} else {
		skip_rows = options.GetSkipRows() + options.dialect_options.header.GetValue();
		skip_bom = skip_rows == 0;
	}

	auto &manager = *source.buffer_manager;
	size = manager.file_handle->FileSize();
	if (source.sniffing || !manager.HasKnownBufferRanges()) {
		return;
	}
	lean = make_uniq<RowLexer>(RowDialect::DuckDB(grammar), source.state_machine->dialect_options.num_cols,
	                           options.maximum_line_size.GetValue(), !ignore_errors, options.null_padding);
	count_only = (columns.empty() || source.placeholder) && filters.empty() &&
	             source.file_row_number == DConstants::INVALID_INDEX;
	empty_only_null = null_strings.size() == 1 && null_strings[0].empty();
	bool lean_decodes = options.thousands_separator == '\0';
	for (auto &column : columns) {
		lean_decodes = lean_decodes && LeanDecodes(column.parse_type);
	}
	lean_ranges =
	    lean->Eligible() && options.maximum_line_size.GetValue() < (idx_t(1) << 30) && (count_only || lean_decodes);
	lean_batches = lean_ranges && !count_only && !late_materialization;
	for (auto &column : columns) {
		lean_batches = lean_batches && !CsvScan::NeedsFlushCast(column);
	}
	parallel = options.parallel || lean_ranges;
	if (!parallel) {
		return;
	}
	FindStart();
	const idx_t data_size = size - MinValue(start, size);
	idx_t wanted = RANGE_BYTES;
	if (lean_ranges) {
		const idx_t threads = MaxValue<idx_t>(TaskScheduler::QueryThreads(context), 1);
		wanted = MinValue(MaxValue(data_size / (threads * 4), LEAN_RANGE_MIN), LEAN_RANGE_MAX);
	}
	range_bytes = MaxValue<idx_t>(MinValue<idx_t>(options.buffer_size_option.GetValue(), wanted), 1);
	range_count = MaxValue<idx_t>((data_size + range_bytes - 1) / range_bytes, 1);
	transfers.resize(range_count);
	computing.assign(range_count, false);
	starts.assign(range_count + 1, CsvRangeStart());
	starts[0].line_end = start;
	starts[0].last_position = start;
}

void CsvScanGlobal::FindStart() {
	auto handle = Buffer(0);
	if (handle && handle->actual_size >= size) {
		whole_file = handle;
	}
	if (skip_rows == 0) {
		start = skip_bom ? SkipBom(handle->Ptr(), handle->actual_size) : 0;
		start_lines = 0;
		return;
	}
	CsvSkip skip(grammar, skip_rows);
	idx_t offset = 0;
	const idx_t buffer_count = source.buffer_manager->KnownBufferCount();
	for (idx_t buffer = 1; handle; buffer++) {
		if (skip.Feed(handle->Ptr(), offset, offset + handle->actual_size) || handle->is_last_buffer) {
			break;
		}
		offset += handle->actual_size;
		handle = buffer < buffer_count ? Buffer(buffer) : nullptr;
	}
	start = MinValue(skip.Start(options), size);
	start_lines = skip.Lines();
}

idx_t CsvScanGlobal::Read(char *data, idx_t count, idx_t offset) const {
	auto &manager = *source.buffer_manager;
	if (manager.HasKnownBufferRanges()) {
		const idx_t available = offset < size ? MinValue(count, size - offset) : 0;
		if (available > 0) {
			manager.file_handle->ReadAt(data, available, offset);
		}
		return available;
	}
	const idx_t buffer_bytes = manager.GetBufferSize();
	idx_t read = 0;
	while (read < count) {
		const idx_t position = offset + read;
		auto handle = manager.GetBuffer(position / buffer_bytes);
		const idx_t within = position % buffer_bytes;
		if (!handle || within >= handle->actual_size) {
			break;
		}
		const idx_t piece = MinValue(count - read, handle->actual_size - within);
		memcpy(data + read, handle->Ptr() + within, piece);
		read += piece;
	}
	return read;
}

void CsvScanGlobal::ReadInto(CsvBytes &bytes, idx_t offset, idx_t count) const {
	count = offset < size ? MinValue(count, size - offset) : 0;
	if (whole_file) {
		bytes.buffer = whole_file;
		bytes.data = whole_file->Ptr() + offset;
		bytes.offset = offset;
		bytes.size = count;
		return;
	}
	if (bytes.capacity < count || !bytes.owned) {
		bytes.capacity = MaxValue<idx_t>(count, 1);
		bytes.owned = make_unsafe_uniq_array_uninitialized<char>(bytes.capacity);
	}
	bytes.buffer.reset();
	bytes.data = bytes.owned.get();
	bytes.offset = offset;
	bytes.size = count ? Read(bytes.owned.get(), count, offset) : 0;
}

CsvRangeBounds CsvScanGlobal::Bounds(idx_t range, CsvBytes &bytes) const {
	const idx_t begin = RangeBegin(range);
	const idx_t end = RangeEnd(range);
	const idx_t from = begin >= 3 ? begin - 3 : 0;
	idx_t want = end - from + RANGE_OVERLAP;
	ReadInto(bytes, from, want);
	const auto sync = [&](idx_t position) {
		while (true) {
			const idx_t available = bytes.offset + bytes.size;
			const bool eof = available >= size;
			const idx_t found = CsvSync(grammar, bytes.data, bytes.offset, position, available, eof);
			if (found != DConstants::INVALID_INDEX) {
				return found;
			}
			if (eof) {
				return size;
			}
			want *= 2;
			ReadInto(bytes, from, want);
		}
	};
	CsvRangeBounds bounds;
	bounds.begin = range == 0 ? start : sync(begin - 1);
	bounds.end = range + 1 == range_count ? size : sync(end - 1);
	bounds.end = MaxValue(bounds.end, bounds.begin);
	return bounds;
}

CSVState CsvScanGlobal::Origin(const CsvBytes &bytes, idx_t begin) const {
	if (begin == start) {
		return CSVState::NOT_SET;
	}
	const char newline = bytes.data[begin - 1 - bytes.offset];
	return newline == '\r' && grammar.carry_on_newline ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR;
}

CsvRangeEntry CsvScanGlobal::Transfer(idx_t range, CSVState origin) const {
	CsvBytes bytes;
	const auto bounds = Bounds(range, bytes);
	return CsvTransfer(grammar, bytes.data, bytes.offset, bounds.begin, bounds.end, origin);
}

idx_t CsvScanGlobal::SyncAt(idx_t position) const {
	const idx_t from = position >= 2 ? position - 2 : 0;
	idx_t want = RANGE_OVERLAP;
	CsvBytes bytes;
	while (true) {
		ReadInto(bytes, from, want);
		const idx_t available = bytes.offset + bytes.size;
		const bool eof = available >= size;
		const idx_t found = CsvSync(grammar, bytes.data, bytes.offset, position, available, eof);
		if (found != DConstants::INVALID_INDEX) {
			return found;
		}
		if (eof) {
			return size;
		}
		want *= 2;
	}
}

void CsvScanGlobal::ViewWholeFile(CsvBytes &bytes) const {
	bytes.owned.reset();
	bytes.capacity = 0;
	bytes.buffer = whole_file;
	bytes.data = whole_file->Ptr();
	bytes.offset = 0;
	bytes.size = size;
}

bool CsvScanGlobal::Claim(idx_t &range) {
	const absl::MutexLock guard(&lock);
	if (!parallel) {
		if (sequential_claimed) {
			return false;
		}
		sequential_claimed = true;
		range = 0;
		return true;
	}
	if (next_range >= range_count) {
		return false;
	}
	range = next_range++;
	return true;
}

void CsvScanGlobal::Computing(idx_t range, bool value) {
	const absl::MutexLock guard(&lock);
	computing[range] = value;
}

vector<unique_ptr<AsyncTask>> CsvScanGlobal::LoadTasks(idx_t range) const {
	return vector<unique_ptr<AsyncTask>>();
}

bool CsvScanGlobal::FinishRange(idx_t range, idx_t lines) {
	source.error_handler->Insert(range, lines);
	const absl::MutexLock guard(&lock);
	finished_ranges++;
	return finished_ranges == MaxValue<idx_t>(parallel ? range_count : 1, 1);
}

bool CsvScanGlobal::AllFinished() const {
	const absl::MutexLock guard(&lock);
	return finished_ranges == (parallel ? range_count : 1);
}

void CsvScanGlobal::Publish(idx_t range, CSVState origin, const CsvRangeEntry &entry) {
	const absl::MutexLock guard(&lock);
	auto &slot = transfers[range][static_cast<uint8_t>(origin)];
	if (!slot.computed) {
		slot = entry;
	}
	computing[range] = false;
	while (resolved < range_count) {
		auto &current = starts[resolved];
		auto &found = transfers[resolved][static_cast<uint8_t>(current.states[1])];
		if (!found.computed) {
			break;
		}
		auto &next = starts[resolved + 1];
		next = found.end;
		next.Inherit(current);
		resolved++;
	}
}

CsvRangeEntry CsvScanGlobal::Entry(idx_t range, CSVState origin) const {
	const absl::MutexLock guard(&lock);
	return transfers[range][static_cast<uint8_t>(origin)];
}

CsvRangeStart CsvScanGlobal::StartState(idx_t range) {
	while (true) {
		idx_t missing;
		CSVState origin;
		{
			const absl::MutexLock guard(&lock);
			auto ready = [&]() {
				return resolved >= range || !computing[resolved];
			};
			lock.Await(absl::Condition(&ready));
			if (resolved >= range) {
				return starts[range];
			}
			missing = resolved;
			origin = starts[missing].states[1];
			computing[missing] = true;
		}
		absl::Cleanup stop = [&] {
			Computing(missing, false);
		};
		Publish(missing, origin, Transfer(missing, origin));
	}
}

CsvScan::CsvScan(CsvScanGlobal &global_p)
    : global(global_p), source(global_p.source), options(global_p.options), grammar(global_p.grammar) {
	splitter = make_uniq<CsvSplitter>(grammar, source.state_machine->dialect_options.num_cols, STANDARD_VECTOR_SIZE);
	value_errors.resize(STANDARD_VECTOR_SIZE);
	failed.resize(STANDARD_VECTOR_SIZE);
	dropped.resize(STANDARD_VECTOR_SIZE);
	selected.resize(STANDARD_VECTOR_SIZE);
	keep.Initialize(STANDARD_VECTOR_SIZE);
	filter_candidates.Initialize(STANDARD_VECTOR_SIZE);
	for (auto &filter : global.filters) {
		filter_states.push_back(TableFilterState::Initialize(global.context, filter.filter.get()));
	}
	auto &allocator = Allocator::Get(global.context);
	vector<LogicalType> parse_types(global.columns.size(), LogicalType::VARCHAR);
	if (!parse_types.empty()) {
		parse_chunk.Initialize(allocator, parse_types);
	}
	scratch_chunk.Initialize(allocator, vector<LogicalType> {LogicalType::VARCHAR});
}

void CsvScan::PublishOwnTransfer() {
	if (!global.parallel || range + 1 >= global.range_count) {
		return;
	}
	global.Computing(range, true);
	absl::Cleanup stop = [&] {
		global.Computing(range, false);
	};
	CsvBytes span;
	const auto bounds = global.Bounds(range, span);
	const bool eof = bounds.end == global.size;
	if (!grammar.quote) {
		global.Publish(range, lean_origin,
		               CsvTransfer(grammar, span.data, span.offset, bounds.begin, bounds.end, lean_origin));
		return;
	}
	CsvRangeEntry quoted;
	RowCursor probe;
	global.lean->Start(probe, bounds.begin, true, false);
	probe.in_quotes = 1;
	const bool lean_quoted =
	    !grammar.carry_on_newline && global.lean->Feed(probe, span.data, span.offset, bounds.end, true, nullptr) &&
	    global.lean->Finish(probe, span.data, span.offset, bounds.end, eof, nullptr, CSVState::QUOTED, quoted);
	if (!lean_quoted) {
		quoted = CsvTransfer(grammar, span.data, span.offset, bounds.begin, bounds.end, CSVState::QUOTED);
	}
	global.Publish(range, CSVState::QUOTED, quoted);
	if (quoted.first_boundary != DConstants::INVALID_INDEX) {
		global.Publish(range, CSVState::QUOTED_NEW_LINE, quoted);
		auto own = CsvTransfer(grammar, span.data, span.offset, bounds.begin, quoted.first_boundary, lean_origin);
		if (own.end.states[1] == CSVState::RECORD_SEPARATOR && own.end.last_position == quoted.first_boundary) {
			own.end = quoted.end;
			global.Publish(range, lean_origin, own);
			return;
		}
	}
	global.Publish(range, lean_origin,
	               CsvTransfer(grammar, span.data, span.offset, bounds.begin, bounds.end, lean_origin));
}

bool CsvScan::LeanFromChain() {
	PublishOwnTransfer();
	const auto start = global.StartState(range);
	const auto state = start.states[1];
	if (state == lean_origin) {
		lean_line_end = start.line_end;
		StartLean(true);
		return true;
	}
	auto entry = global.Entry(range, state);
	if (!entry.computed) {
		entry = global.Transfer(range, state);
		global.Publish(range, state, entry);
	}
	if (lean_stop == DConstants::INVALID_INDEX) {
		lean_stop = MaxValue(global.SyncAt(global.RangeEnd(range) - 1), lean_start);
	}
	if (entry.first_boundary == DConstants::INVALID_INDEX || entry.first_boundary >= lean_stop) {
		return false;
	}
	auto boundary = entry.boundary;
	boundary.Inherit(start);
	lean_start = entry.first_boundary;
	lean_origin = boundary.states[1];
	lean_line_end = boundary.line_end;
	StartLean(true);
	return true;
}

void CsvScan::Start(idx_t range_p) {
	range = range_p;
	done = false;
	started = false;
	reported = false;
	no_rows = false;
	lean = false;
	lean_rows = DConstants::INVALID_INDEX;
	lean_lines = DConstants::INVALID_INDEX;
	if (!global.parallel) {
		bytes.reset();
	}
	base = 0;
	pending.reset();
	pending_start = 0;
	pending_used = 0;
	next_buffer = 0;
	next_buffer_start = 0;
	stitch_bytes = 0;
	last_buffer = false;
	range_begin = 0;
	range_end = NumericLimits<idx_t>::Maximum();
}

void CsvScan::View(shared_ptr<CSVBufferHandle> handle, idx_t position) {
	auto view = make_shared_ptr<CsvBytes>();
	view->data = handle->Ptr();
	view->size = handle->actual_size;
	view->buffer = std::move(handle);
	bytes = std::move(view);
	base = position;
	stitch_bytes = 0;
}

bool CsvScan::ReadMore(idx_t keep) {
	if (global.parallel) {
		if (last_buffer) {
			return false;
		}
		const idx_t span_end = base + bytes->size;
		const idx_t from = MinValue(MaxValue(keep, base), span_end);
		stitch_bytes = MaxValue(stitch_bytes * 2, STITCH_BYTES);
		auto next = make_shared_ptr<CsvBytes>();
		global.ReadInto(*next, from, span_end - from + stitch_bytes);
		bytes = std::move(next);
		base = bytes->offset;
		last_buffer = base + bytes->size >= global.size;
		return true;
	}
	if (!pending) {
		if (last_buffer) {
			return false;
		}
		auto &manager = *source.buffer_manager;
		auto handle = manager.GetBuffer(next_buffer);
		if (!handle) {
			last_buffer = true;
			return false;
		}
		if (!source.sniffing) {
			*source.bytes_read += handle->actual_size;
			if (next_buffer > 0) {
				manager.ResetBuffer(next_buffer - 1);
			}
			if (handle->is_last_buffer) {
				manager.ResetBuffer(next_buffer);
			}
		}
		pending = std::move(handle);
		pending_start = next_buffer_start;
		pending_used = 0;
		next_buffer++;
		next_buffer_start += pending->actual_size;
	}
	const idx_t span_end = bytes ? base + bytes->size : pending_start;
	keep = bytes ? MinValue(MaxValue(keep, base), span_end) : pending_start;
	if (keep >= pending_start) {
		const bool is_last = pending->is_last_buffer;
		View(std::move(pending), pending_start);
		last_buffer = is_last;
		return true;
	}
	stitch_bytes = MaxValue(stitch_bytes * 2, STITCH_BYTES);
	const idx_t piece = MinValue(stitch_bytes, pending->actual_size - pending_used);
	const idx_t kept = span_end - keep;
	auto stitched = make_shared_ptr<CsvBytes>();
	stitched->owned = make_unsafe_uniq_array_uninitialized<char>(MaxValue<idx_t>(kept + piece, 1));
	memcpy(stitched->owned.get(), bytes->data + (keep - base), kept);
	memcpy(stitched->owned.get() + kept, pending->Ptr() + pending_used, piece);
	stitched->data = stitched->owned.get();
	stitched->size = kept + piece;
	bytes = std::move(stitched);
	base = keep;
	pending_used += piece;
	if (pending_used == pending->actual_size) {
		last_buffer = pending->is_last_buffer;
		pending.reset();
	}
	return true;
}

void CsvScan::SkipRows() {
	ReadMore(0);
	if (global.skip_rows == 0) {
		splitter->Reset(bytes && global.skip_bom ? SkipBom(bytes->data, bytes->size) : 0, 0);
		return;
	}
	CsvSkip skip(grammar, global.skip_rows);
	bool skipped = false;
	while (bytes) {
		skipped = skip.Feed(bytes->data, base, base + bytes->size);
		if (skipped || !ReadMore(base + bytes->size)) {
			break;
		}
	}
	if (!skipped && source.sniffing) {
		bytes.reset();
		pending.reset();
		next_buffer = 0;
		next_buffer_start = 0;
		last_buffer = false;
		ReadMore(0);
		splitter->Reset(0, 0);
		return;
	}
	splitter->Reset(skip.Start(options), skip.Lines());
}

bool CsvScan::ReadRange() {
	if (!bytes || bytes.use_count() > 1) {
		bytes = make_shared_ptr<CsvBytes>();
	}
	CSVState origin;
	{
		global.Computing(range, true);
		absl::Cleanup stop = [&] {
			global.Computing(range, false);
		};
		const auto bounds = global.Bounds(range, *bytes);
		base = bytes->offset;
		stitch_bytes = 0;
		last_buffer = base + bytes->size >= global.size;
		range_begin = bounds.begin;
		range_end = bounds.end;
		origin = global.Origin(*bytes, range_begin);
		global.Publish(range, origin, CsvTransfer(grammar, bytes->data, base, range_begin, range_end, origin));
	}
	const auto start = global.StartState(range);
	if (range_begin >= range_end) {
		return false;
	}
	if (range == 0) {
		splitter->Reset(range_begin, global.start_lines);
		return true;
	}
	const auto state = start.states[1];
	if (state == origin) {
		splitter->Reset(range_begin, 0, start);
		return true;
	}
	const auto entry = CsvTransfer(grammar, bytes->data, base, range_begin, range_end, state);
	if (entry.first_boundary == DConstants::INVALID_INDEX) {
		return false;
	}
	auto boundary = entry.boundary;
	boundary.Inherit(start);
	splitter->Reset(entry.first_boundary, 0, boundary);
	return true;
}

void CsvScan::StartLean(bool second_pass) {
	lean = true;
	lean_extract = !global.count_only;
	lean_probe_only = false;
	lean_finished = false;
	lean_verified = false;
	lean_second_pass = second_pass;
	lean_failed = false;
	chunk_open = false;
	chunk_rows = 0;
	held = nullptr;
	lean_rows = DConstants::INVALID_INDEX;
	lean_lines = DConstants::INVALID_INDEX;
	emitted_rows = 0;
	last_emitted_end = DConstants::INVALID_INDEX;
	fields.Clear();
	if (!second_pass) {
		lean_start = DConstants::INVALID_INDEX;
		lean_stop = range + 1 == global.range_count ? global.size : DConstants::INVALID_INDEX;
		lean_line_end = range == 0 ? global.start : DConstants::INVALID_INDEX;
	}
	const idx_t begin = global.RangeBegin(range);
	const idx_t from = second_pass ? lean_start : (range == 0 ? global.start : begin - MinValue<idx_t>(begin, 3));
	if (!bytes || bytes.use_count() > 1) {
		bytes = make_shared_ptr<CsvBytes>();
	}
	if (global.WholeFile()) {
		global.ViewWholeFile(*bytes);
		read_to = global.size;
	} else {
		if (bytes->buffer) {
			bytes = make_shared_ptr<CsvBytes>();
		}
		bytes->offset = from;
		bytes->size = 0;
		bytes->data = bytes->owned.get();
		read_to = from;
	}
	if (second_pass) {
		global.lean->Start(cursor, lean_start, true);
		validated_to = lean_start;
	}
}

idx_t CsvScan::KeepFrom() const {
	if (lean_start == DConstants::INVALID_INDEX) {
		return bytes->offset;
	}
	idx_t keep = cursor.pos;
	const idx_t first_row = global.lean_batches && chunk_open ? chunk_rows : 0;
	if (lean_extract && !lean_probe_only && fields.row_count > first_row) {
		keep = MinValue<idx_t>(keep, lean_start + fields.rows[first_row]);
	}
	if (!lean_extract && !global.columns.empty()) {
		keep = MinValue(keep, validated_to);
	}
	if (lean_stop == DConstants::INVALID_INDEX) {
		const idx_t end = global.RangeEnd(range);
		keep = MinValue(keep, end - MinValue<idx_t>(end, 3));
	}
	return MaxValue(keep, bytes->offset);
}

void CsvScan::PrepareWindow(idx_t keep_from, idx_t need) {
	auto &window = *bytes;
	const idx_t kept = read_to - keep_from;
	const bool fits = window.owned && read_to - window.offset + need <= window.capacity;
	if (bytes.use_count() == 1 && window.owned && window.capacity >= kept + need) {
		if (keep_from > window.offset && (kept <= LEAN_PIECE || !fits)) {
			memmove(window.owned.get(), window.owned.get() + (keep_from - window.offset), kept);
			window.offset = keep_from;
			window.size = kept;
		}
		window.data = window.owned.get();
		return;
	}
	if (fits) {
		return;
	}
	shared_ptr<CsvBytes> next;
	if (spare && spare.use_count() == 1) {
		next = std::move(spare);
	} else {
		next = make_shared_ptr<CsvBytes>();
	}
	if (!next->owned || next->capacity < kept + need) {
		const idx_t capacity = MaxValue(kept + need, MaxValue(LEAN_WINDOW, window.capacity * 2));
		next->owned = make_unsafe_uniq_array_uninitialized<char>(capacity);
		next->capacity = capacity;
	}
	if (kept) {
		memcpy(next->owned.get(), window.data + (keep_from - window.offset), kept);
	}
	next->buffer.reset();
	next->data = next->owned.get();
	next->offset = keep_from;
	next->size = kept;
	if (bytes.use_count() > 1) {
		spare = std::move(bytes);
	}
	bytes = std::move(next);
}

void CsvScan::ReadLeanPiece() {
	const idx_t want = MinValue<idx_t>(LEAN_PIECE, global.size - read_to);
	PrepareWindow(KeepFrom(), want);
	auto &window = *bytes;
	const idx_t got = want ? global.Read(window.owned.get() + (read_to - window.offset), want, read_to) : 0;
	window.size += got;
	read_to += got;
}

bool CsvScan::CheckCountUtf8(idx_t end) {
	if (global.columns.empty()) {
		return true;
	}
	if (!cursor.non_ascii) {
		validated_to = cursor.pos;
		return true;
	}
	if (end <= validated_to) {
		return true;
	}
	if (!simdutf::validate_utf8(bytes->data + (validated_to - bytes->offset), end - validated_to)) {
		return false;
	}
	validated_to = end;
	return true;
}

bool CsvScan::AdvanceLean() {
	const bool eof = read_to >= global.size;
	if (lean_start == DConstants::INVALID_INDEX) {
		idx_t found = global.start;
		if (range > 0) {
			const idx_t from = global.RangeBegin(range) - 1;
			found = read_to > from ? CsvSync(grammar, bytes->data, bytes->offset, from, read_to, eof)
			                       : DConstants::INVALID_INDEX;
			if (found == DConstants::INVALID_INDEX) {
				if (!eof) {
					ReadLeanPiece();
					return true;
				}
				found = global.size;
			}
		}
		if (lean_stop != DConstants::INVALID_INDEX) {
			found = MinValue(found, lean_stop);
		}
		lean_start = found;
		lean_origin = found == global.start ? CSVState::NOT_SET : global.Origin(*bytes, found);
		if (found > global.start) {
			idx_t run = found;
			while (run > bytes->offset &&
			       (bytes->data[run - 1 - bytes->offset] == '\n' || bytes->data[run - 1 - bytes->offset] == '\r')) {
				run--;
			}
			lean_line_end = run > bytes->offset ? run : DConstants::INVALID_INDEX;
		}
		global.lean->Start(cursor, lean_start, range == 0);
		validated_to = lean_start;
		return true;
	}
	if (lean_stop == DConstants::INVALID_INDEX) {
		const idx_t from = global.RangeEnd(range) - 1;
		if (read_to > from) {
			const idx_t found = CsvSync(grammar, bytes->data, bytes->offset, from, read_to, eof);
			if (found != DConstants::INVALID_INDEX) {
				lean_stop = MaxValue(found, lean_start);
			} else if (eof) {
				lean_stop = global.size;
			}
		}
	}
	const idx_t limit = lean_stop != DConstants::INVALID_INDEX ? lean_stop : global.RangeEnd(range);
	const idx_t feed_end = MinValue(limit, read_to);
	const bool last = lean_stop != DConstants::INVALID_INDEX && feed_end == lean_stop;
	if (cursor.pos < feed_end && (last || feed_end - cursor.pos >= CSV_BLOCK_BYTES + global.lean->Lookahead())) {
		auto out = lean_extract && !lean_probe_only ? &fields : nullptr;
		if (!global.lean->Feed(cursor, bytes->data, bytes->offset, feed_end, last, out)) {
			return false;
		}
		if (!lean_extract &&
		    !CheckCountUtf8(cursor.last_term == DConstants::INVALID_INDEX ? lean_start : cursor.last_term + 1)) {
			return false;
		}
		if (out && !cursor.settled && read_to - KeepFrom() > LEAN_UNSETTLED) {
			lean_probe_only = true;
			fields.Clear();
		}
		return true;
	}
	if (lean_stop != DConstants::INVALID_INDEX && cursor.pos == lean_stop) {
		return FinishLean();
	}
	ReadLeanPiece();
	return true;
}

bool CsvScan::FinishLean() {
	CsvRangeEntry entry;
	auto out = lean_extract && !lean_probe_only ? &fields : nullptr;
	if (!global.lean->Finish(cursor, bytes->data, bytes->offset, lean_stop, lean_stop == global.size, out, lean_origin,
	                         entry)) {
		return false;
	}
	if (!lean_extract && !CheckCountUtf8(lean_stop)) {
		return false;
	}
	lean_finished = true;
	if (!lean_second_pass) {
		global.Publish(range, lean_origin, entry);
	}
	lean_lines = cursor.lines + (range == 0 ? global.start_lines : 0);
	if (!lean_extract) {
		lean_rows = cursor.rows;
	}
	return true;
}

void CsvScan::LeanToExact() {
	lean = false;
	lean_lines = DConstants::INVALID_INDEX;
	lean_rows = DConstants::INVALID_INDEX;
	const bool settled = cursor.settled || lean_verified;
	if (!settled && !lean_second_pass) {
		if (!LeanFromChain()) {
			done = true;
			no_rows = true;
		}
		return;
	}
	const idx_t line_end = emitted_rows > 0 ? last_emitted_end : lean_line_end;
	if (lean_probe_only || line_end == DConstants::INVALID_INDEX) {
		if (!ReadRange()) {
			done = true;
			no_rows = true;
		}
		return;
	}
	CsvRangeStart start;
	idx_t restart;
	idx_t lines = 0;
	auto state = CSVState::RECORD_SEPARATOR;
	if (!lean_extract) {
		restart = lean_start;
	} else if (fields.row_count > 0) {
		restart = lean_start + fields.rows[0];
	} else {
		restart = cursor.pos;
		if (grammar.carry_on_newline && cursor.after_cr) {
			state = CSVState::CARRIAGE_RETURN;
		}
	}
	if (restart == lean_start) {
		state = lean_origin;
	} else {
		lines = cursor.lines - cursor.unterminated;
		if (restart < cursor.pos) {
			lines -= global.lean->CountLines(bytes->data, bytes->offset, restart, cursor.pos);
		}
	}
	if (!lean_finished) {
		PublishOwnTransfer();
	}
	start.states[0] = state;
	start.states[1] = state;
	start.line_end = line_end;
	start.last_position = restart;
	if (range == 0) {
		lines += global.start_lines;
	}
	if (lean_stop == DConstants::INVALID_INDEX) {
		lean_stop = MaxValue(global.SyncAt(global.RangeEnd(range) - 1), lean_start);
	}
	if (restart < bytes->offset) {
		bytes = make_shared_ptr<CsvBytes>();
		global.ReadInto(*bytes, restart, LEAN_WINDOW);
		read_to = bytes->offset + bytes->size;
	}
	base = bytes->offset;
	stitch_bytes = 0;
	range_begin = lean_start;
	range_end = lean_stop;
	last_buffer = read_to >= global.size;
	splitter->Reset(restart, lines, start);
}

bool CsvScan::ScanLean(DataChunk &output, idx_t max_rows) {
	const idx_t columns = splitter->Columns();
	while (lean) {
		const bool settled = cursor.settled || lean_verified;
		if (lean_finished && !settled) {
			const auto start = global.StartState(range);
			if (start.states[1] != lean_origin) {
				lean = false;
				lean_lines = DConstants::INVALID_INDEX;
				lean_rows = DConstants::INVALID_INDEX;
				if (!LeanFromChain()) {
					done = true;
					no_rows = true;
					return false;
				}
				continue;
			}
			lean_verified = true;
			continue;
		}
		if (lean_finished && lean_probe_only) {
			StartLean(true);
			continue;
		}
		if (!lean_extract) {
			if (!lean_finished) {
				if (!AdvanceLean()) {
					LeanToExact();
					if (lean) {
						continue;
					}
					return false;
				}
				continue;
			}
			if (lean_rows == 0) {
				lean = false;
				done = true;
				return false;
			}
			const idx_t count = MinValue(lean_rows, max_rows);
			lean_rows -= count;
			output.Reset();
			for (auto &column : output.data) {
				column.SetVectorType(VectorType::CONSTANT_VECTOR);
				ConstantVector::SetNull(column, true);
			}
			output.SetChildCardinality(count);
			return true;
		}
		const idx_t ready = fields.end_count / columns;
		if (!global.lean_batches) {
			if (settled && ready > 0 && (ready >= max_rows || lean_finished)) {
				const idx_t count = MinValue(ready, max_rows);
				if (!DecodeLean(count, output)) {
					LeanToExact();
					if (lean) {
						continue;
					}
					return false;
				}
				last_emitted_end = lean_start + (fields.ends[count * columns - 1] & ROW_FIELD_MASK);
				fields.Drop(count, count * columns);
				emitted_rows += count;
				if (output.size() > 0) {
					return true;
				}
				continue;
			}
			if (lean_finished) {
				lean = false;
				done = true;
				return false;
			}
			if (!AdvanceLean()) {
				LeanToExact();
				if (lean) {
					continue;
				}
				return false;
			}
			continue;
		}
		if (lean_failed && !chunk_open) {
			lean_failed = false;
			LeanToExact();
			if (lean) {
				continue;
			}
			return false;
		}
		if (!chunk_open) {
			output.Reset();
			chunk_rows = 0;
			held = nullptr;
			chunk_open = true;
		}
		const idx_t limit = MinValue(ready, max_rows);
		if (settled && !lean_failed && chunk_rows < limit) {
			if (DecodeLeanBatch(chunk_rows, limit - chunk_rows, output)) {
				chunk_rows = limit;
				continue;
			}
			lean_failed = true;
		}
		if (chunk_rows > 0 && (lean_failed || chunk_rows == max_rows || (lean_finished && chunk_rows == ready))) {
			LeanRowNumbers(chunk_rows, output);
			if (!global.filters.empty()) {
				for (idx_t row = 0; row < chunk_rows; row++) {
					selected[row] = true;
				}
				Filter(chunk_rows, output);
			}
			FinishLeanChunk(chunk_rows, output);
			last_emitted_end = lean_start + (fields.ends[chunk_rows * columns - 1] & ROW_FIELD_MASK);
			fields.Drop(chunk_rows, chunk_rows * columns);
			emitted_rows += chunk_rows;
			chunk_open = false;
			if (output.size() > 0) {
				return true;
			}
			continue;
		}
		if (lean_failed) {
			chunk_open = false;
			continue;
		}
		if (lean_finished) {
			chunk_open = false;
			lean = false;
			done = true;
			return false;
		}
		if (!AdvanceLean()) {
			lean_failed = true;
		}
	}
	return false;
}

idx_t CsvScan::Lookup(std::span<const int64_t> offsets, std::span<idx_t> survivors, DataChunk &output) {
	D_ASSERT(offsets.size() <= splitter->Capacity());
	auto arena = make_shared_ptr<CsvBytes>();
	idx_t capacity = 0;
	idx_t window_begin = 0;
	idx_t window_end = 0;
	idx_t window_offset = 0;
	bool window_eof = false;
	auto read = [&](idx_t from, idx_t want) {
		if (arena->size + want > capacity) {
			capacity = MaxValue(arena->size + want, capacity * 2);
			auto grown = make_unsafe_uniq_array_uninitialized<char>(capacity);
			if (arena->size) {
				memcpy(grown.get(), arena->owned.get(), arena->size);
			}
			arena->owned = std::move(grown);
			arena->data = arena->owned.get();
		}
		const idx_t got = global.Read(arena->owned.get() + arena->size, want, from);
		arena->size += got;
		window_end = from + got;
		window_eof = got < want;
	};
	splitter->ClearRows();
	lookup_offsets = offsets;
	lookup_rows.clear();
	idx_t count = 0;
	for (idx_t i = 0; i < offsets.size(); i++) {
		const auto offset = static_cast<idx_t>(offsets[i]);
		if (offsets[i] < 0 || offset < global.start) {
			continue;
		}
		const idx_t first = offset > global.start ? offset - 1 : offset;
		if (first < window_begin || offset >= window_end) {
			if (window_end > window_begin && !window_eof && first >= window_end && first - window_end < LOOKUP_BYTES) {
				read(window_end, offset + LOOKUP_BYTES - window_end);
			} else {
				window_offset = arena->size;
				window_begin = first;
				read(first, offset + LOOKUP_BYTES - first);
			}
			if (offset >= window_end) {
				continue;
			}
		}
		const idx_t position = window_offset + (offset - window_begin);
		CsvRangeStart start;
		start.line_end = position;
		start.last_position = position;
		if (offset > global.start) {
			const bool carriage_return = arena->data[position - 1] == '\r' && grammar.carry_on_newline;
			start.states[1] = carriage_return ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR;
			start.line_end = position - 1;
			start.last_position = position + carriage_return;
		}
		splitter->Reset(position, 0, start);
		idx_t grow = LOOKUP_BYTES;
		while (true) {
			const idx_t arena_end = window_offset + (window_end - window_begin);
			const auto rows = splitter->Split(arena->data, 0, arena_end, count + 1, window_eof,
			                                  NumericLimits<idx_t>::Maximum(), true);
			if (rows > count || splitter->Finished() || window_eof) {
				count = rows;
				break;
			}
			read(window_end, grow);
			grow *= 2;
		}
		while (lookup_rows.size() < count) {
			lookup_rows.push_back(i);
		}
	}
	bytes = arena;
	base = 0;
	idx_t found = 0;
	if (count > 0) {
		Decode(count, output);
		for (idx_t row = 0; row < count; row++) {
			if (!failed[row] && !dropped[row] && selected[row]) {
				survivors[found++] = lookup_rows[row];
			}
		}
	}
	output.SetCardinality(found);
	lookup_offsets = {};
	bytes.reset();
	return found;
}

bool CsvScan::IsNull(const CsvField &field, const char *value, idx_t size) const {
	const bool quoted = field.flags & CSV_FIELD_QUOTED;
	if (quoted && !options.allow_quoted_nulls) {
		return false;
	}
	const bool escaped_null = !grammar.strict_mode && (field.flags & CSV_FIELD_ESCAPED) && !quoted && size == 1;
	for (auto &null_string : global.null_strings) {
		if (null_string.size() == 2 && null_string[0] == grammar.escape) {
			if (escaped_null && null_string[1] == value[0]) {
				return true;
			}
		} else if (size == null_string.size() && !escaped_null && memcmp(null_string.c_str(), value, size) == 0) {
			return true;
		}
	}
	return false;
}

bool CsvScan::DecodeValue(const CsvScanColumn &column, const CsvField &field, Vector &vector, idx_t row,
                          CsvRowError &error) {
	const char *value = bytes->data + field.offset;
	idx_t size = field.size;
	const auto parse_id = column.parse_type.id();
	string_t unescaped;
	if (field.flags & CSV_FIELD_ESCAPED) {
		if (parse_id != LogicalTypeId::VARCHAR) {
			error = {CSVErrorType::CAST_ERROR, column.file_column, base + field.start,
			         global.ignore_errors ? string() : CastMessage(value, size, LogicalTypeIdToString(parse_id))};
			return false;
		}
		unescaped = CsvRemoveEscape(value, size, grammar.escape, grammar.quote, grammar.strict_mode, vector);
		value = unescaped.GetData();
		size = unescaped.GetSize();
	}
	if (IsNull(field, value, size)) {
		if (column.force_not_null) {
			if (parse_id != LogicalTypeId::VARCHAR) {
				error = {CSVErrorType::CAST_ERROR, column.file_column, base + field.start, string()};
				return false;
			}
			FlatVector::GetDataMutable<string_t>(vector)[row] = string_t();
		} else {
			FlatVector::SetNull(vector, row, true);
		}
		return true;
	}
	string thousands;
	if ((LogicalType::IsNumeric(parse_id) || parse_id == LogicalTypeId::BIGNUM) &&
	    options.thousands_separator != '\0') {
		thousands = CsvStripSeparator(value, size, options.thousands_separator);
		value = thousands.c_str();
		size = thousands.size();
	}
	bool success = true;
	if (parse_id == LogicalTypeId::BIGNUM) {
		while (size > 0 && StringUtil::CharacterIsSpace(*value)) {
			value++;
			size--;
		}
		while (size > 0 && StringUtil::CharacterIsSpace(value[size - 1])) {
			size--;
		}
		try {
			auto bignum = Bignum::VarcharToBignum(string_t(value, NumericCast<uint32_t>(size)));
			FlatVector::GetDataMutable<string_t>(vector)[row] =
			    StringVector::AddStringOrBlob(vector, bignum.data(), bignum.size());
		} catch (const ConversionException &) {
			success = false;
		}
	} else if (parse_id != LogicalTypeId::VARCHAR) {
		const ParseContext context {&global.date_format, &global.timestamp_format, value + size,
		                            global.decimal_separator};
		success = WithParser(column.parse_type, context, [&](auto parsed, auto parse) {
			using T = typename decltype(parsed)::type;
			return parse(value, size, FlatVector::GetDataMutable<T>(vector)[row]);
		});
	} else {
		auto &out = FlatVector::GetDataMutable<string_t>(vector)[row];
		if (column.validate_utf8 && !Utf8Proc::IsValid(value, UnsafeNumericCast<uint32_t>(size))) {
			out = string_t();
			error = {CSVErrorType::INVALID_ENCODING, column.file_column, base + field.start, string()};
			return false;
		}
		if ((field.flags & CSV_FIELD_ESCAPED) || !thousands.empty()) {
			out = StringVector::AddStringOrBlob(vector, value, size);
		} else {
			out = string_t(value, UnsafeNumericCast<uint32_t>(size));
		}
	}
	if (!success) {
		error = {CSVErrorType::CAST_ERROR, column.file_column, base + field.start,
		         global.ignore_errors ? string() : CastMessage(value, size, LogicalTypeIdToString(parse_id))};
		return false;
	}
	return true;
}

string CsvScan::LineText(const CsvRow &row, bool &first_nl) const {
	if (row.begin < base) {
		first_nl = true;
	}
	if (!global.print_line || row.begin == row.end || row.end <= base) {
		return string();
	}
	const char *data = bytes->data;
	idx_t start = 0;
	if (row.begin >= base) {
		start = row.begin - base;
		first_nl = data[start] == '\n' || data[start] == '\r';
		start += first_nl;
	}
	const idx_t stop = MinValue(row.end - base, bytes->size);
	if (start >= stop) {
		return string();
	}
	string line(data + start, stop - start);
	CsvSanitize(line);
	return line;
}

void CsvScan::ReportRow(idx_t row_idx, vector<CsvRowError> &errors) {
	if (source.sniffing) {
		for (auto &error : errors) {
			if (error.type != CSVErrorType::INVALID_ENCODING) {
				continue;
			}
			if (options.ignore_errors.GetValue()) {
				return;
			}
			auto encoding = std::move(error);
			errors.clear();
			errors.push_back(std::move(encoding));
			break;
		}
	}
	auto &row = splitter->Row(row_idx);
	bool first_nl = false;
	auto line = LineText(row, first_nl);
	const LinesPerBoundary lines(range, row.lines);
	const auto row_position = row.begin + first_nl;
	const auto &names = source.names;
	const String path(source.path);
	for (auto &error : errors) {
		const optional_idx error_position(error.position == row.begin ? error.position + first_nl : error.position);
		CSVError csv_error;
		switch (error.type) {
		case CSVErrorType::TOO_MANY_COLUMNS:
		case CSVErrorType::TOO_FEW_COLUMNS:
			csv_error = CSVError::IncorrectColumnAmountError(options, error.column, lines, line, row_position,
			                                                 error_position, path);
			break;
		case CSVErrorType::INVALID_ENCODING:
			csv_error = CSVError::InvalidUTF8(options, error.column, lines, line, row_position, error_position, path);
			break;
		case CSVErrorType::UNTERMINATED_QUOTES:
			csv_error = CSVError::UnterminatedQuotesError(options, error.column, lines, line, row_position,
			                                              error_position, path);
			break;
		case CSVErrorType::CAST_ERROR: {
			string column_name;
			LogicalTypeId type_id = LogicalTypeId::INVALID;
			if (error.column < names.size()) {
				column_name = names[error.column];
			}
			for (auto &column : global.columns) {
				if (column.file_column == error.column) {
					type_id = column.parse_type.id();
				}
			}
			csv_error = CSVError::CastError(options, column_name, error.message, error.column, line, lines,
			                                row_position, error_position, type_id, path);
			break;
		}
		case CSVErrorType::MAXIMUM_LINE_SIZE:
			csv_error = CSVError::LineSizeError(options, lines, line, row_position, path);
			break;
		case CSVErrorType::INVALID_STATE:
			csv_error = CSVError::InvalidState(options, error.column, lines, line, row_position, error_position, path);
			break;
		default:
			throw InvalidInputException("CSV Error not allowed when inserting row");
		}
		const bool force = error.type == CSVErrorType::INVALID_STATE ||
		                   (source.sniffing && error.type == CSVErrorType::INVALID_ENCODING);
		Report(row_idx, csv_error, force);
	}
}

void CsvScan::Report(idx_t row, CSVError &error, bool force) {
	if (lookup_offsets.empty()) {
		source.error_handler->Error(error, force);
		return;
	}
	throw InvalidInputException("Could not read the CSV row at byte %llu of \"%s\": %s",
	                            lookup_offsets[lookup_rows[row]], source.path, error.error_message);
}

bool CsvScan::NeedsFlushCast(const CsvScanColumn &column) {
	auto &type = column.type;
	return type.IsJSONType() ||
	       (type.id() != LogicalTypeId::VARCHAR && column.parse_type.id() == LogicalTypeId::VARCHAR);
}

bool CsvScan::EvaluatesOnFieldBytes(const Expression &expr) {
	vector<CsvRawPredicate> predicates;
	if (expr.GetExpressionType() == ExpressionType::OPERATOR_NOT) {
		auto &children = expr.Cast<BoundOperatorExpression>().GetChildren();
		return children.size() == 1 && CollectRawPattern(*children[0], true, predicates);
	}
	return CollectRawPattern(expr, false, predicates);
}

void CsvScan::FlushCasts(idx_t count, DataChunk &output, bool late, bool filter_pass) {
	const auto &names = source.names;
	const String path(source.path);
	for (idx_t i = 0; i < global.columns.size(); i++) {
		auto &column = global.columns[i];
		if (!NeedsFlushCast(column) || column.output_column >= output.ColumnCount()) {
			continue;
		}
		if (late && global.filtered[i] != filter_pass) {
			continue;
		}
		auto &parse = parse_chunk.data[i];
		auto &result = output.data[column.output_column];
		string error_message;
		if (VectorOperations::TryCast(global.context, parse, result, count, &error_message, false, true)) {
			continue;
		}
		auto &result_validity = FlatVector::Validity(result);
		auto &parse_validity = FlatVector::Validity(parse);
		bool first = true;
		const idx_t lines_now = splitter->LinesRead();
		for (idx_t row = 0; row < count; row++) {
			if (dropped[row] || result_validity.RowIsValid(row) || !parse_validity.RowIsValid(row)) {
				continue;
			}
			failed[row] = true;
			if (!global.ignore_errors) {
				auto &csv_row = splitter->Row(row);
				bool first_nl = false;
				auto line = LineText(csv_row, first_nl);
				auto value = FlatVector::GetData<string_t>(parse)[row];
				auto message = CastMessage(value.GetData(), value.GetSize(),
				                           first ? column.type.ToString() : LogicalTypeIdToString(column.type.id()));
				idx_t row_position = csv_row.begin + first_nl;
				if (first && csv_row.begin == csv_row.end) {
					row_position = 0;
				}
				const LinesPerBoundary lines(range, lines_now - count + row);
				auto csv_error =
				    CSVError::CastError(options, names[column.file_column], message, column.file_column, line, lines,
				                        row_position, optional_idx::Invalid(), column.type.id(), path);
				Report(row, csv_error, false);
			}
			first = false;
		}
	}
}

void CsvScan::RowErrors(idx_t row_idx, vector<CsvRowError> &row_value_errors, vector<CsvRowError> &errors) {
	auto &row = splitter->Row(row_idx);
	const idx_t num_cols = splitter->Columns();
	auto &events = splitter->Events();
	auto &extras = splitter->Extras();
	idx_t event = event_cursor;
	while (event < events.size() && events[event].row < row_idx) {
		event++;
	}
	idx_t value = 0;
	auto add_events_until = [&](idx_t column) {
		while (event < events.size() && events[event].row == row_idx && events[event].column <= column) {
			auto &e = events[event];
			errors.push_back({e.kind == CsvRowEventKind::UNTERMINATED_QUOTES ? CSVErrorType::UNTERMINATED_QUOTES
			                                                                 : CSVErrorType::INVALID_STATE,
			                  e.column, e.position, string()});
			event++;
		}
	};
	for (idx_t column = 0; column < MinValue<idx_t>(row.field_count, num_cols); column++) {
		add_events_until(column);
		while (value < row_value_errors.size() && row_value_errors[value].column == column) {
			errors.push_back(std::move(row_value_errors[value++]));
		}
	}
	if (row.field_count > num_cols && !grammar.strict_mode) {
		used_unstrictness = true;
	}
	if (row.field_count > num_cols && grammar.strict_mode) {
		while (extra_cursor < extras.size() && extras[extra_cursor].row < row_idx) {
			extra_cursor++;
		}
		for (idx_t extra = extra_cursor; extra < extras.size() && extras[extra].row == row_idx; extra++) {
			auto &field = extras[extra].field;
			add_events_until(extras[extra].column);
			if (extras[extra].column == num_cols && (!(field.flags & CSV_FIELD_QUOTED) || options.allow_quoted_nulls)) {
				const char *data = bytes->data + field.offset;
				bool is_null = false;
				for (auto &null_string : global.null_strings) {
					is_null = is_null ||
					          (field.size == null_string.size() && memcmp(null_string.c_str(), data, field.size) == 0);
				}
				if (is_null) {
					continue;
				}
			}
			errors.push_back({CSVErrorType::TOO_MANY_COLUMNS, extras[extra].column, base + field.start, string()});
		}
	}
	add_events_until(NumericLimits<idx_t>::Maximum());
	if (row.flags & CSV_ROW_UNFINISHED) {
		return;
	}
	if (row.end - row.begin > options.maximum_line_size.GetValue()) {
		errors.push_back({CSVErrorType::MAXIMUM_LINE_SIZE, 1, row.last_position, string()});
	}
	if (!options.null_padding) {
		for (idx_t column = row.field_count; column < num_cols; column++) {
			errors.push_back({CSVErrorType::TOO_FEW_COLUMNS, column - 1, row.last_position, string()});
		}
	}
}

Vector &CsvScan::Target(idx_t column, DataChunk &output) {
	auto &scan_column = global.columns[column];
	if (NeedsFlushCast(scan_column)) {
		return parse_chunk.data[column];
	}
	if (scan_column.output_column < output.ColumnCount()) {
		return output.data[scan_column.output_column];
	}
	return scratch_chunk.data[0];
}

void CsvScan::DecodeColumn(idx_t column_idx, idx_t count, DataChunk &output, bool selected_only) {
	auto &column = global.columns[column_idx];
	auto &vector = Target(column_idx, output);
	if (vector.GetType().InternalType() == PhysicalType::VARCHAR) {
		StringVector::AddAuxiliaryData(vector, make_uniq<CsvBytesHolder>(bytes));
	}
	if (column.parse_type.id() == LogicalTypeId::VARCHAR) {
		ExactStrings(column, vector, count, selected_only);
		return;
	}
	if (options.thousands_separator == '\0' && LeanDecodes(column.parse_type)) {
		const ParseContext context {&global.date_format, &global.timestamp_format, bytes->data + bytes->size,
		                            global.decimal_separator};
		WithParser(column.parse_type, context, [&](auto parsed, auto parse) {
			using T = typename decltype(parsed)::type;
			ExactValues<T>(column, vector, count, selected_only, parse);
			return true;
		});
		return;
	}
	for (idx_t row = 0; row < count; row++) {
		if (selected_only && !selected[row]) {
			FlatVector::SetNull(vector, row, true);
			continue;
		}
		DecodeCell(column, vector, row);
	}
}

void CsvScan::DecodeCell(const CsvScanColumn &column, Vector &vector, idx_t row) {
	auto &csv_row = splitter->Row(row);
	if (csv_row.flags & CSV_ROW_EMPTY_LINE) {
		if (!global.empty_is_null) {
			dropped[row] = true;
			FlatVector::SetNull(vector, row, true);
		} else if (!options.force_not_null.empty() && options.force_not_null[0] &&
		           vector.GetType().InternalType() == PhysicalType::VARCHAR) {
			FlatVector::GetDataMutable<string_t>(vector)[row] = string_t();
		} else {
			FlatVector::SetNull(vector, row, true);
		}
		return;
	}
	if (column.file_column >= csv_row.field_count) {
		const bool padded_empty =
		    options.null_padding && column.force_not_null && vector.GetType().InternalType() == PhysicalType::VARCHAR;
		if (padded_empty) {
			FlatVector::GetDataMutable<string_t>(vector)[row] = string_t();
		} else {
			FlatVector::SetNull(vector, row, true);
		}
		return;
	}
	CsvRowError error;
	if (!DecodeValue(column, splitter->Column(column.file_column)[row], vector, row, error)) {
		FlatVector::SetNull(vector, row, true);
		value_errors[row].push_back(std::move(error));
	}
}

template <class T, class PARSE>
void CsvScan::ExactValues(const CsvScanColumn &column, Vector &vector, idx_t count, bool selected_only, PARSE parse) {
	T *const out = FlatVector::GetDataMutable<T>(vector);
	const CsvField *const fields = splitter->Column(column.file_column);
	const char *const data = bytes->data;
	const bool empty_only_null = global.empty_only_null;
	const bool allow_quoted_nulls = options.allow_quoted_nulls;
	const bool force_not_null = column.force_not_null;
	const idx_t file_column = column.file_column;
	for (idx_t row = 0; row < count; row++) {
		if (selected_only && !selected[row]) {
			FlatVector::SetNull(vector, row, true);
			continue;
		}
		const auto &csv_row = splitter->Row(row);
		const auto &field = fields[row];
		if (!(csv_row.flags & CSV_ROW_EMPTY_LINE) && file_column < csv_row.field_count &&
		    !(field.flags & CSV_FIELD_ESCAPED)) {
			const char *value = data + field.offset;
			const idx_t size = field.size;
			if (empty_only_null) {
				if (size != 0) {
					if (parse(value, size, out[row])) {
						continue;
					}
				} else if (!force_not_null && (!(field.flags & CSV_FIELD_QUOTED) || allow_quoted_nulls)) {
					FlatVector::SetNull(vector, row, true);
					continue;
				}
			} else if (!IsNull(field, value, size) && parse(value, size, out[row])) {
				continue;
			}
		}
		DecodeCell(column, vector, row);
	}
}

void CsvScan::ExactStrings(const CsvScanColumn &column, Vector &vector, idx_t count, bool selected_only) {
	string_t *const out = FlatVector::GetDataMutable<string_t>(vector);
	const CsvField *const fields = splitter->Column(column.file_column);
	const char *const data = bytes->data;
	const char *const limit = bytes->data + bytes->size;
	const bool empty_only_null = global.empty_only_null;
	const bool allow_quoted_nulls = options.allow_quoted_nulls;
	const bool validate = column.validate_utf8;
	const idx_t file_column = column.file_column;
	for (idx_t row = 0; row < count; row++) {
		if (selected_only && !selected[row]) {
			FlatVector::SetNull(vector, row, true);
			continue;
		}
		const auto &csv_row = splitter->Row(row);
		const auto &field = fields[row];
		if (!(csv_row.flags & CSV_ROW_EMPTY_LINE) && file_column < csv_row.field_count &&
		    !(field.flags & CSV_FIELD_ESCAPED)) {
			const char *value = data + field.offset;
			const idx_t size = field.size;
			const bool null = empty_only_null ? size == 0 && (!(field.flags & CSV_FIELD_QUOTED) || allow_quoted_nulls)
			                                  : IsNull(field, value, size);
			if (!null) {
				if (!validate || Utf8Proc::IsValid(value, UnsafeNumericCast<uint32_t>(size))) {
					MakeString(value, size, limit, out[row]);
					continue;
				}
			} else if (!column.force_not_null) {
				FlatVector::SetNull(vector, row, true);
				continue;
			}
		}
		DecodeCell(column, vector, row);
	}
}

void CsvScan::FillRowNumbers(idx_t count, DataChunk &output) {
	if (source.file_row_number == DConstants::INVALID_INDEX || source.file_row_number >= output.ColumnCount()) {
		return;
	}
	auto data = FlatVector::GetDataMutable<int64_t>(output.data[source.file_row_number]);
	for (idx_t row = 0; row < count; row++) {
		data[row] =
		    lookup_offsets.empty() ? static_cast<int64_t>(splitter->Row(row).start) : lookup_offsets[lookup_rows[row]];
	}
}

void CsvScan::Filter(idx_t count, DataChunk &output) {
	idx_t approved = 0;
	for (idx_t row = 0; row < count; row++) {
		if (selected[row]) {
			filter_candidates.set_index(approved++, row);
		}
	}
	filter_selection.Initialize(filter_candidates);
	for (idx_t i = 0; i < global.filters.size() && approved > 0; i++) {
		auto &vector = output.data[global.filters[i].output_column];
		FlatVector::SetSize(vector, count_t(count));
		ColumnSegment::FilterSelection(filter_selection, vector, *filter_states[i], count, approved);
	}
	for (idx_t row = 0; row < count; row++) {
		selected[row] = false;
	}
	for (idx_t i = 0; i < approved; i++) {
		selected[filter_selection.get_index(i)] = true;
	}
}

void CsvScan::Decode(idx_t count, DataChunk &output) {
	output.Reset();
	parse_chunk.Reset();
	scratch_chunk.Reset();
	const idx_t num_cols = splitter->Columns();
	const bool filters = !global.filters.empty();
	const bool late = global.late_materialization;
	for (idx_t row = 0; row < count; row++) {
		failed[row] = false;
		dropped[row] = false;
		selected[row] = true;
		value_errors[row].clear();
	}
	const bool raw = late && ExactRawFilter(count);
	for (idx_t i = 0; i < global.columns.size(); i++) {
		if (!late || global.filtered[i]) {
			DecodeColumn(i, count, output, raw);
		}
	}
	if (late) {
		FlushCasts(count, output, true, true);
		FillRowNumbers(count, output);
		Filter(count, output);
		for (idx_t i = 0; i < global.columns.size(); i++) {
			if (!global.filtered[i]) {
				DecodeColumn(i, count, output, true);
			}
		}
	}
	event_cursor = 0;
	extra_cursor = 0;
	for (idx_t row = 0; row < count; row++) {
		auto &csv_row = splitter->Row(row);
		if (dropped[row] || (csv_row.flags & CSV_ROW_EMPTY_LINE)) {
			continue;
		}
		const bool may_fail = !value_errors[row].empty() || (csv_row.flags & CSV_ROW_EVENTS) ||
		                      csv_row.field_count != num_cols ||
		                      csv_row.end - csv_row.begin > options.maximum_line_size.GetValue();
		if (!may_fail) {
			continue;
		}
		row_errors.clear();
		RowErrors(row, value_errors[row], row_errors);
		if (row_errors.empty()) {
			continue;
		}
		failed[row] = true;
		if (!global.ignore_errors) {
			ReportRow(row, row_errors);
		}
	}
	FlushCasts(count, output, late, false);
	if (!late) {
		FillRowNumbers(count, output);
		if (filters) {
			Filter(count, output);
		}
	}
	idx_t kept = 0;
	for (idx_t row = 0; row < count; row++) {
		if (!failed[row] && !dropped[row] && selected[row]) {
			keep.set_index(kept++, row);
		}
	}
	output.SetChildCardinality(count);
	if (kept != count) {
		output.Slice(keep, kept);
	}
	output.SetChildCardinality(kept);
}

LeanSource CsvScan::LeanSourceFor(const CsvScanColumn &column, idx_t first, bool selected_only) const {
	const idx_t columns = splitter->Columns();
	LeanSource source;
	source.first = first;
	if (column.file_column == 0) {
		source.begins = fields.rows.get();
		source.begin_stride = 1;
		source.begin_bias = 0;
	} else {
		source.begins = fields.ends.get() + column.file_column - 1;
		source.begin_stride = columns;
		source.begin_bias = grammar.delimiter_size;
	}
	source.ends = fields.ends.get() + column.file_column;
	source.stride = columns;
	source.data = bytes->data;
	source.limit = bytes->data + bytes->size;
	source.shift = lean_start - bytes->offset;
	source.quote = grammar.quote;
	source.escape_probe = grammar.escape && grammar.escape != grammar.quote ? grammar.escape : grammar.quote;
	source.quotes = cursor.quotes;
	source.doubled = cursor.doubled;
	source.padded = cursor.padded;
	source.spaced = global.lean->Dialect().quoted_spaces;
	source.pad_empty = column.force_not_null;
	source.allow_quoted_nulls = options.allow_quoted_nulls;
	source.empty_only_null = global.empty_only_null;
	source.null_strings = &global.null_strings;
	source.selected = selected_only ? &selected : nullptr;
	return source;
}

bool CsvScan::LeanRawFilter(idx_t count) {
	bool applied = false;
	for (auto &filter : global.filters) {
		if (filter.raw.empty()) {
			continue;
		}
		applied = true;
		LeanRawPredicates(LeanSourceFor(global.columns[filter.column], 0, false), count, filter.raw, selected);
	}
	return applied;
}

bool CsvScan::ExactRawFilter(idx_t count) {
	bool applied = false;
	for (auto &filter : global.filters) {
		if (filter.raw.empty()) {
			continue;
		}
		applied = true;
		auto &column = global.columns[filter.column];
		const CsvField *const column_fields = splitter->Column(column.file_column);
		for (idx_t row = 0; row < count; row++) {
			if (!selected[row]) {
				continue;
			}
			auto &csv_row = splitter->Row(row);
			auto &field = column_fields[row];
			if ((csv_row.flags & CSV_ROW_EMPTY_LINE) || column.file_column >= csv_row.field_count ||
			    (field.flags & CSV_FIELD_ESCAPED)) {
				continue;
			}
			const char *value = bytes->data + field.offset;
			const bool null = global.empty_only_null
			                      ? field.size == 0 && (!(field.flags & CSV_FIELD_QUOTED) || options.allow_quoted_nulls)
			                      : IsNull(field, value, field.size);
			selected[row] = RawPasses(filter.raw, value, field.size, null);
		}
	}
	return applied;
}

bool CsvScan::DecodeLeanColumn(idx_t column_idx, idx_t first, idx_t count, DataChunk &output, bool selected_only,
                               bool utf8_valid, bool hold) {
	auto &column = global.columns[column_idx];
	auto &vector = Target(column_idx, output);
	const auto source = LeanSourceFor(column, first, selected_only);
	const bool force = column.force_not_null;
	if (column.parse_type.id() == LogicalTypeId::VARCHAR) {
		if (hold) {
			StringVector::AddAuxiliaryData(vector, make_uniq<CsvBytesHolder>(bytes));
		}
		return LeanStrings(source, count, column, column.validate_utf8 && !utf8_valid, grammar, vector);
	}
	if (!LeanDecodes(column.parse_type)) {
		return false;
	}
	const ParseContext context {&global.date_format, &global.timestamp_format, source.limit, global.decimal_separator};
	return WithParser(column.parse_type, context, [&](auto parsed, auto parse) {
		using T = typename decltype(parsed)::type;
		return LeanValues<T>(source, count, force, vector, parse);
	});
}

bool CsvScan::LeanCasts(idx_t count, DataChunk &output, bool late, bool filter_pass) {
	for (idx_t i = 0; i < global.columns.size(); i++) {
		auto &column = global.columns[i];
		if (!NeedsFlushCast(column) || column.output_column >= output.ColumnCount()) {
			continue;
		}
		if (late && global.filtered[i] != filter_pass) {
			continue;
		}
		string error_message;
		if (!VectorOperations::TryCast(global.context, parse_chunk.data[i], output.data[column.output_column], count,
		                               &error_message, false, true)) {
			return false;
		}
	}
	return true;
}

void CsvScan::LeanRowNumbers(idx_t count, DataChunk &output) {
	if (source.file_row_number == DConstants::INVALID_INDEX || source.file_row_number >= output.ColumnCount()) {
		return;
	}
	auto data = FlatVector::GetDataMutable<int64_t>(output.data[source.file_row_number]);
	const idx_t columns = splitter->Columns();
	for (idx_t row = 0; row < count; row++) {
		data[row] = static_cast<int64_t>(lean_start + fields.rows[row]);
	}
}

bool CsvScan::DecodeLean(idx_t count, DataChunk &output) {
	output.Reset();
	parse_chunk.Reset();
	scratch_chunk.Reset();
	const idx_t columns = splitter->Columns();
	bool utf8_valid = !cursor.non_ascii;
	if (!utf8_valid) {
		const idx_t first = lean_start + fields.rows[0];
		const idx_t last = lean_start + (fields.ends[count * columns - 1] & ROW_FIELD_MASK);
		utf8_valid = simdutf::validate_utf8(bytes->data + (first - bytes->offset), last - first);
	}
	const bool late = global.late_materialization;
	for (idx_t row = 0; row < count; row++) {
		selected[row] = true;
	}
	const bool raw = late && LeanRawFilter(count);
	for (idx_t i = 0; i < global.columns.size(); i++) {
		if ((!late || global.filtered[i]) && !DecodeLeanColumn(i, 0, count, output, raw, utf8_valid, true)) {
			return false;
		}
	}
	if (late) {
		if (!LeanCasts(count, output, true, true)) {
			return false;
		}
		LeanRowNumbers(count, output);
		Filter(count, output);
		for (idx_t i = 0; i < global.columns.size(); i++) {
			if (!global.filtered[i] && !DecodeLeanColumn(i, 0, count, output, true, utf8_valid, true)) {
				return false;
			}
		}
	}
	if (!LeanCasts(count, output, late, false)) {
		return false;
	}
	if (!late) {
		LeanRowNumbers(count, output);
		if (!global.filters.empty()) {
			Filter(count, output);
		}
	}
	FinishLeanChunk(count, output);
	return true;
}

bool CsvScan::DecodeLeanBatch(idx_t first, idx_t count, DataChunk &output) {
	const idx_t columns = splitter->Columns();
	bool utf8_valid = !cursor.non_ascii;
	if (!utf8_valid) {
		const idx_t from = lean_start + fields.rows[first];
		const idx_t to = lean_start + (fields.ends[(first + count) * columns - 1] & ROW_FIELD_MASK);
		utf8_valid = simdutf::validate_utf8(bytes->data + (from - bytes->offset), to - from);
	}
	const bool hold = bytes.get() != held;
	for (idx_t i = 0; i < global.columns.size(); i++) {
		if (!DecodeLeanColumn(i, first, count, output, false, utf8_valid, hold)) {
			return false;
		}
	}
	held = bytes.get();
	return true;
}

void CsvScan::FinishLeanChunk(idx_t count, DataChunk &output) {
	output.SetChildCardinality(count);
	if (!global.filters.empty()) {
		idx_t kept = 0;
		for (idx_t row = 0; row < count; row++) {
			if (selected[row]) {
				keep.set_index(kept++, row);
			}
		}
		if (kept != count) {
			output.Slice(keep, kept);
		}
		output.SetChildCardinality(kept);
	}
}

bool CsvScan::Scan(DataChunk &output, idx_t max_rows) {
	source.error_handler->ErrorIfNeeded();
	if (!started) {
		started = true;
		if (global.parallel) {
			if (global.lean_ranges) {
				StartLean(false);
			} else if (!ReadRange()) {
				done = true;
				no_rows = true;
			}
		} else {
			SkipRows();
		}
	}
	if (lean && ScanLean(output, max_rows)) {
		return false;
	}
	while (!done) {
		idx_t count = 0;
		if (bytes) {
			count = splitter->Split(bytes->data, base, base + bytes->size, max_rows, last_buffer, range_end);
		}
		if (count > 0) {
			Decode(count, output);
			if (output.size() > 0) {
				return false;
			}
			continue;
		}
		if (splitter->Finished() || last_buffer) {
			done = true;
			break;
		}
		ReadMore(splitter->KeepFrom());
	}
	output.Reset();
	if (reported) {
		return false;
	}
	reported = true;
	if (global.parallel) {
		*source.bytes_read += global.RangeEnd(range) - global.RangeBegin(range);
	} else {
		bytes.reset();
	}
	pending.reset();
	if (source.sniffing) {
		return true;
	}
	const idx_t lines = no_rows ? 0 : (lean_lines != DConstants::INVALID_INDEX ? lean_lines : splitter->LinesRead());
	return global.FinishRange(range, lines);
}

} // namespace duckdb
