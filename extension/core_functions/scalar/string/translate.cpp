#include "core_functions/scalar/string_functions.hpp"

#include "duckdb/common/array.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "utf8proc.hpp"
#include "utf8proc_wrapper.hpp"

#include <absl/container/flat_hash_map.h>

namespace duckdb {

static constexpr idx_t TRANSLATE_CHECK_BYTES = 1 << 20;
static constexpr int32_t TRANSLATE_UNMAPPED = -2;
static constexpr int32_t TRANSLATE_DELETE = -1;

struct TranslateMap {
	TranslateMap(const string_t &needle, const string_t &thread) {
		auto input_needle = needle.GetData();
		auto size_needle = needle.GetSize();

		auto input_thread = thread.GetData();
		auto size_thread = thread.GetSize();

		ascii.fill(TRANSLATE_UNMAPPED);
		idx_t i = 0, j = 0;
		int sz = 0;

		// Character to be replaced
		while (i < size_needle && j < size_thread) {
			auto codepoint_needle = Utf8Proc::UTF8ToCodepoint(input_needle, sz, size_needle - i);
			input_needle += sz;
			i += UnsafeNumericCast<idx_t>(sz);
			auto codepoint_thread = Utf8Proc::UTF8ToCodepoint(input_thread, sz, size_thread - j);
			input_thread += sz;
			j += UnsafeNumericCast<idx_t>(sz);
			Map(codepoint_needle, codepoint_thread);
		}

		// Character to be deleted
		while (i < size_needle) {
			auto codepoint_needle = Utf8Proc::UTF8ToCodepoint(input_needle, sz, size_needle - i);
			input_needle += sz;
			i += UnsafeNumericCast<idx_t>(sz);
			Map(codepoint_needle, TRANSLATE_DELETE);
		}
	}

	void Map(int32_t from, int32_t to) {
		if (from >= 0 && from < 128) {
			if (ascii[from] == TRANSLATE_UNMAPPED) {
				ascii[from] = to;
			}
		} else {
			other.try_emplace(from, to);
		}
	}

	int32_t Find(int32_t codepoint) const {
		if (codepoint >= 0 && codepoint < 128) {
			return ascii[codepoint];
		}
		auto entry = other.find(codepoint);
		return entry == other.end() ? TRANSLATE_UNMAPPED : entry->second;
	}

	array<int32_t, 128> ascii;
	absl::flat_hash_map<int32_t, int32_t> other;
};

static string_t TranslateScalarFunction(const string_t &haystack, const TranslateMap &map, vector<char> &result,
                                        ExpressionState &state) {
	auto input_haystack = haystack.GetData();
	auto size_haystack = haystack.GetSize();

	// Reuse the buffer
	result.clear();
	result.reserve(size_haystack);

	int sz = 0, c_sz = 0;
	char c[5] = {'\0', '\0', '\0', '\0', '\0'};
	idx_t unchecked = 0;
	for (idx_t i = 0; i < size_haystack; i += UnsafeNumericCast<idx_t>(sz)) {
		int32_t target;
		const auto byte = static_cast<unsigned char>(*input_haystack);
		if (byte < 128) {
			sz = 1;
			target = map.ascii[byte];
		} else {
			target = map.Find(Utf8Proc::UTF8ToCodepoint(input_haystack, sz, size_haystack - i));
		}
		if (target == TRANSLATE_UNMAPPED) {
			result.insert(result.end(), input_haystack, input_haystack + sz);
		} else if (target != TRANSLATE_DELETE) {
			Utf8Proc::CodepointToUtf8(target, c_sz, c);
			result.insert(result.end(), c, c + c_sz);
		}
		input_haystack += sz;
		unchecked += UnsafeNumericCast<idx_t>(sz);
		if (unchecked >= TRANSLATE_CHECK_BYTES) {
			state.InterruptCheck();
			unchecked = 0;
		}
	}

	if (result.size() > string_t::MAX_STRING_SIZE) {
		throw OutOfRangeException(
		    "Cannot create a string of size: '%d' in TRANSLATE, the maximum supported string size is: '%d'",
		    result.size(), string_t::MAX_STRING_SIZE);
	}
	return string_t(result.data(), UnsafeNumericCast<uint32_t>(result.size()));
}

static void TranslateFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &haystack_vector = args.data[0];
	const auto &needle_vector = args.data[1];
	const auto &thread_vector = args.data[2];

	vector<char> buffer;
	auto &heap = StringVector::GetStringHeap(result);
	if (needle_vector.GetVectorType() == VectorType::CONSTANT_VECTOR &&
	    thread_vector.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(needle_vector) &&
	    !ConstantVector::IsNull(thread_vector)) {
		const TranslateMap map(ConstantVector::GetData<string_t>(needle_vector)[0],
		                       ConstantVector::GetData<string_t>(thread_vector)[0]);
		UnaryExecutor::Execute<string_t, string_t>(haystack_vector, result, args.size(), [&](string_t input_string) {
			return heap.AddString(TranslateScalarFunction(input_string, map, buffer, state));
		});
		return;
	}
	TernaryExecutor::Execute<string_t, string_t, string_t, string_t>(
	    haystack_vector, needle_vector, thread_vector, result,
	    [&](string_t input_string, string_t needle_string, string_t thread_string) {
		    const TranslateMap map(needle_string, thread_string);
		    return heap.AddString(TranslateScalarFunction(input_string, map, buffer, state));
	    });
}

ScalarFunction TranslateFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::VARCHAR, TranslateFunction);
	fun.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("from", LogicalType::VARCHAR)
	    .AddParameter("to", LogicalType::VARCHAR);
	fun.SetFallible();
	return fun;
}

} // namespace duckdb
