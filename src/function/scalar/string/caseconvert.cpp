#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/function/scalar/string_common.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

#include "utf8proc_wrapper.hpp"

#include <absl/strings/ascii.h>

#include <string.h>

namespace duckdb {

template <bool IS_UPPER>
static string_t ASCIICaseConvert(StringHeap &heap, const char *input_data, idx_t input_length) {
	idx_t output_length = input_length;
	auto result_str = heap.EmptyString(output_length);
	auto result_data = result_str.GetDataWriteable();
	if constexpr (IS_UPPER) {
		absl::ascii_internal::AsciiStrToUpper(result_data, input_data, input_length);
	} else {
		absl::ascii_internal::AsciiStrToLower(result_data, input_data, input_length);
	}
	result_str.Finalize();
	return result_str;
}

namespace {

template <bool IS_UPPER>
int32_t ConvertCodepoint(int32_t codepoint) {
	return IS_UPPER ? Utf8Proc::CodepointToUpper(codepoint) : Utf8Proc::CodepointToLower(codepoint);
}

template <bool IS_UPPER>
char ConvertAscii(char c) {
	return UnsafeNumericCast<char>(IS_UPPER ? absl::ascii_toupper(static_cast<uint8_t>(c))
	                                        : absl::ascii_tolower(static_cast<uint8_t>(c)));
}

template <bool IS_UPPER>
uint64_t ConvertAsciiWord(const char *input, char *output) {
	auto word = Load<uint64_t>(const_data_ptr_cast(input));
	Store<uint64_t>(IS_UPPER ? SwarWord::UpperAscii(word) : SwarWord::LowerAscii(word), data_ptr_cast(output));
	return word;
}

template <bool IS_UPPER>
idx_t ConvertAsciiRun(const char *input, idx_t i, idx_t size, idx_t words_end, char *output) {
	for (; i + SwarWord::SIZE <= words_end; i += SwarWord::SIZE) {
		auto high = ConvertAsciiWord<IS_UPPER>(input + i, output + i) & SwarWord::MSB;
		if (high) {
			return i + SwarWord::FirstFlagged(high);
		}
	}
	if (i < size && words_end == size && size >= SwarWord::SIZE) {
		auto last = size - SwarWord::SIZE;
		if (SwarWord::IsAscii(Load<uint64_t>(const_data_ptr_cast(input + last)))) {
			ConvertAsciiWord<IS_UPPER>(input + last, output + last);
			return size;
		}
	}
	for (; i < size && !(input[i] & 0x80); i++) {
		output[i] = ConvertAscii<IS_UPPER>(input[i]);
	}
	return i;
}

template <bool IS_UPPER>
struct CaseTable {
	CaseTable() : two_byte(), changing_blocks() {
		for (int32_t codepoint = 0x80; codepoint < 0x800; codepoint++) {
			auto converted = ConvertCodepoint<IS_UPPER>(codepoint);
			if (converted < 0x80 || converted >= 0x800) {
				continue;
			}
			const char bytes[] = {static_cast<char>(0xC0 | (converted >> 6)),
			                      static_cast<char>(0x80 | (converted & 0x3F))};
			memcpy(&two_byte[codepoint - 0x80], bytes, sizeof(bytes));
		}
		for (int32_t codepoint = 0x800; codepoint < 0x10000; codepoint++) {
			if ((codepoint >= 0xD800 && codepoint < 0xE000) || ConvertCodepoint<IS_UPPER>(codepoint) != codepoint) {
				changing_blocks[codepoint >> 14] |= uint64_t(1) << ((codepoint >> 8) & 63);
			}
		}
	}

	static const CaseTable &Get() {
		static const CaseTable table;
		return table;
	}

	bool Changing(int32_t codepoint) const {
		return (changing_blocks[codepoint >> 14] >> ((codepoint >> 8) & 63)) & 1;
	}

	uint16_t two_byte[0x800 - 0x80];
	uint64_t changing_blocks[4];
};

template <bool IS_UPPER>
DUCKDB_NOINLINE idx_t ConvertCodepointSameSize(const char *input, idx_t size, char *output) {
	int input_size = 0;
	auto codepoint = Utf8Proc::UTF8ToCodepoint(input, input_size, size);
	char converted[4];
	int converted_size = 0;
	if (!Utf8Proc::CodepointToUtf8(ConvertCodepoint<IS_UPPER>(codepoint), converted_size, converted) ||
	    converted_size != input_size) {
		return 0;
	}
	memcpy(output, converted, UnsafeNumericCast<idx_t>(converted_size));
	return UnsafeNumericCast<idx_t>(converted_size);
}

template <bool IS_UPPER>
[[gnu::always_inline]] inline idx_t ConvertSameSize(const CaseTable<IS_UPPER> &table, const char *input, idx_t size,
                                                    char *output) {
	auto lead = static_cast<uint8_t>(input[0]);
	if (lead >= 0xC2 && lead < 0xE0 && size >= 2 && !IsCharacter(input[1])) {
		auto codepoint = (lead & 0x1F) << 6 | (static_cast<uint8_t>(input[1]) & 0x3F);
		auto bytes = table.two_byte[codepoint - 0x80];
		if (!bytes) {
			return 0;
		}
		memcpy(output, &bytes, sizeof(bytes));
		return sizeof(bytes);
	}
	if (lead >= 0xE0 && lead < 0xF0 && size >= 3 && !IsCharacter(input[1]) && !IsCharacter(input[2])) {
		auto codepoint = (lead & 0x0F) << 12 | (static_cast<uint8_t>(input[1]) & 0x3F) << 6 |
		                 (static_cast<uint8_t>(input[2]) & 0x3F);
		if (codepoint >= 0x800 && !table.Changing(codepoint)) {
			memcpy(output, input, 3);
			return 3;
		}
	}
	return ConvertCodepointSameSize<IS_UPPER>(input, size, output);
}

template <bool IS_UPPER>
idx_t ConvertSameLength(const char *input, idx_t size, char *output, idx_t capacity) {
	auto &table = CaseTable<IS_UPPER>::Get();
	auto words_end = MinValue(size, capacity);
	idx_t i = 0;
	while (i < size) {
		if (!(input[i] & 0x80)) {
			// ascii
			i = ConvertAsciiRun<IS_UPPER>(input, i, size, words_end, output);
			continue;
		}
		auto converted = ConvertSameSize<IS_UPPER>(table, input + i, size - i, output + i);
		if (!converted) {
			return i;
		}
		i += converted;
	}
	return size;
}

} // namespace

template <bool IS_UPPER>
static idx_t GetResultLength(const char *input_data, idx_t input_length) {
	auto &table = CaseTable<IS_UPPER>::Get();
	idx_t output_length = 0;
	for (idx_t i = 0; i < input_length;) {
		if (!(input_data[i] & 0x80)) {
			// ASCII.
			auto ascii = FirstNonAscii(input_data + i, input_length - i);
			output_length += ascii;
			i += ascii;
			continue;
		}

		// UTF-8.
		char converted[4];
		auto same_size = ConvertSameSize<IS_UPPER>(table, input_data + i, input_length - i, converted);
		if (same_size) {
			output_length += same_size;
			i += same_size;
			continue;
		}
		int sz = 0;
		auto codepoint = Utf8Proc::UTF8ToCodepoint(input_data + i, sz, input_length - i);
		auto new_sz = Utf8Proc::CodepointLength(ConvertCodepoint<IS_UPPER>(codepoint));
		output_length += UnsafeNumericCast<idx_t>(new_sz);
		D_ASSERT(sz != 0);
		i += UnsafeNumericCast<idx_t>(sz);
	}
	return output_length;
}

template <bool IS_UPPER>
static idx_t CaseConvert(const char *input_data, idx_t input_length, char *result_data, idx_t result_capacity) {
	idx_t input_pos = 0;
	idx_t result_pos = 0;
	while (true) {
		auto same = ConvertSameLength<IS_UPPER>(input_data + input_pos, input_length - input_pos,
		                                        result_data + result_pos, result_capacity - result_pos);
		input_pos += same;
		result_pos += same;
		if (input_pos == input_length) {
			return result_pos;
		}
		// non-ascii character
		int sz = 0, new_sz = 0;
		auto codepoint = Utf8Proc::UTF8ToCodepoint(input_data + input_pos, sz, input_length - input_pos);
		auto success =
		    Utf8Proc::CodepointToUtf8(ConvertCodepoint<IS_UPPER>(codepoint), new_sz, result_data + result_pos);
		D_ASSERT(success);
		(void)success;
		input_pos += UnsafeNumericCast<idx_t>(sz);
		result_pos += static_cast<idx_t>(new_sz);
	}
}

idx_t LowerLength(const char *input_data, idx_t input_length) {
	return GetResultLength<false>(input_data, input_length);
}

idx_t LowerCase(const char *input_data, idx_t input_length, char *result_data, idx_t result_capacity) {
	return CaseConvert<false>(input_data, input_length, result_data, result_capacity);
}

template <bool IS_UPPER>
static string_t UnicodeCaseConvert(StringHeap &heap, const char *input_data, idx_t input_length) {
	if (FirstNonAscii(input_data, input_length) == input_length) {
		// ASCII: code points and bytes coincide
		return ASCIICaseConvert<IS_UPPER>(heap, input_data, input_length);
	}
	auto same_length = heap.EmptyString(input_length);
	auto same = ConvertSameLength<IS_UPPER>(input_data, input_length, same_length.GetDataWriteable(), input_length);
	if (same == input_length) {
		same_length.Finalize();
		return same_length;
	}
	auto output_length = same + GetResultLength<IS_UPPER>(input_data + same, input_length - same);
	auto result_str = heap.EmptyString(output_length);
	auto result_data = result_str.GetDataWriteable();
	memcpy(result_data, same_length.GetData(), same);
	CaseConvert<IS_UPPER>(input_data + same, input_length - same, result_data + same, output_length - same);
	result_str.Finalize();
	return result_str;
}

namespace {
template <bool IS_UPPER>
struct CaseConvertOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, StringHeap &heap) {
		auto input_data = input.GetData();
		auto input_length = input.GetSize();
		return UnicodeCaseConvert<IS_UPPER>(heap, input_data, input_length);
	}
};
} // namespace

template <bool IS_UPPER>
static void CaseConvertFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::ExecuteString<string_t, string_t, CaseConvertOperator<IS_UPPER>>(args.data[0], result);
}

namespace {
template <bool IS_UPPER>
struct CaseConvertOperatorASCII {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, StringHeap &heap) {
		auto input_data = input.GetData();
		auto input_length = input.GetSize();
		return ASCIICaseConvert<IS_UPPER>(heap, input_data, input_length);
	}
};
} // namespace

template <bool IS_UPPER>
static void CaseConvertFunctionASCII(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::ExecuteString<string_t, string_t, CaseConvertOperatorASCII<IS_UPPER>>(args.data[0], result);
}

template <bool IS_UPPER>
static unique_ptr<BaseStatistics> CaseConvertPropagateStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	D_ASSERT(child_stats.size() == 1);
	// can only propagate stats if the children have stats
	if (!StringStats::CanContainUnicode(child_stats[0])) {
		expr.FunctionMutable().SetFunctionCallback(CaseConvertFunctionASCII<IS_UPPER>);
	}
	// case conversion is not order- or length-preserving, but it never turns a valid string into NULL
	auto result = StringStats::CreateUnknown(expr.GetReturnType());
	result.CopyValidity(child_stats[0]);
	if (!StringStats::HasMinMax(child_stats[0])) {
		return result.ToUnique();
	}
	// All values share the common prefix of min and max; case conversion preserves this property.
	auto min = StringStats::Min(child_stats[0]);
	auto max = StringStats::Max(child_stats[0]);
	const bool is_exact = min == max && StringStats::GetMinType(child_stats[0]) == StringStatsType::EXACT_STATS &&
	                      StringStats::GetMaxType(child_stats[0]) == StringStatsType::EXACT_STATS;
	if (!is_exact) {
		min.resize(StringUtil::GetCommonPrefixSize(min, max));
		// truncated stats can end in the middle of a character - only complete ones can be converted
		size_t invalid_pos = 0;
		if (Utf8Proc::Analyze(min.c_str(), min.size(), nullptr, &invalid_pos) == UnicodeType::INVALID) {
			min.resize(invalid_pos);
		}
		if (min.empty()) {
			return result.ToUnique();
		}
	}
	string converted;
	converted.resize(GetResultLength<IS_UPPER>(min.c_str(), min.size()));
	CaseConvert<IS_UPPER>(min.c_str(), min.size(), &converted[0], converted.size());
	auto stats_type = is_exact ? StringStatsType::EXACT_STATS : StringStatsType::TRUNCATED_STATS;
	// case conversion can lengthen a string beyond what the stats can store
	if (converted.size() > StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE) {
		converted.resize(StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE);
		stats_type = StringStatsType::TRUNCATED_STATS;
	}
	StringStats::SetMin(result, string_t(converted), stats_type);
	StringStats::SetMax(result, string_t(converted), stats_type);
	return result.ToUnique();
}

ScalarFunction LowerFun::GetFunction() {
	ScalarFunction fun("lower", {}, LogicalType::VARCHAR, CaseConvertFunction<false>, nullptr,
	                   CaseConvertPropagateStats<false>);
	fun.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	return fun;
}

ScalarFunction UpperFun::GetFunction() {
	ScalarFunction fun("upper", {}, LogicalType::VARCHAR, CaseConvertFunction<true>, nullptr,
	                   CaseConvertPropagateStats<true>);
	fun.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	return fun;
}

} // namespace duckdb
