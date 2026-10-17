#include "core_functions/scalar/string_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/function/scalar/string_common.hpp"

#include <string.h>
#include <ctype.h>
#include <unordered_map>

namespace duckdb {

static constexpr idx_t REPLACE_CHECK_BYTES = 1 << 20;

static bool ReplaceScalarFunction(ExpressionState &state, const string_t &haystack, const string_t &needle,
                                  const string_t &thread, const duckdb_re2::LiteralFinder &finder,
                                  vector<char> &result) {
	auto input_haystack = haystack.GetData();
	auto size_haystack = haystack.GetSize();
	auto input_needle = const_uchar_ptr_cast(needle.GetData());
	auto size_needle = needle.GetSize();
	const auto find = [&] {
		return FindStrInStr(const_uchar_ptr_cast(input_haystack), size_haystack, input_needle, size_needle, finder);
	};
	auto string_position = find();
	if (string_position == DConstants::INVALID_INDEX) {
		return false;
	}
	auto input_thread = thread.GetData();
	auto size_thread = thread.GetSize();

	auto check_size = [&] {
		if (result.size() > string_t::MAX_STRING_SIZE) {
			throw OutOfRangeException(
			    "Cannot create a string of size: '%d' in REPLACE, the maximum supported string size is: '%d'",
			    result.size(), string_t::MAX_STRING_SIZE);
		}
	};
	result.clear();
	idx_t unchecked = 0;
	for (;; string_position = find()) {
		if (string_position == DConstants::INVALID_INDEX) {
			string_position = size_haystack;
		}
		result.insert(result.end(), input_haystack, input_haystack + string_position);
		input_haystack += string_position;
		size_haystack -= string_position;
		if (size_haystack == 0) {
			break;
		}
		result.insert(result.end(), input_thread, input_thread + size_thread);
		input_haystack += size_needle;
		size_haystack -= size_needle;
		check_size();
		unchecked += string_position + size_needle + size_thread;
		if (unchecked >= REPLACE_CHECK_BYTES) {
			state.InterruptCheck();
			unchecked = 0;
		}
	}
	check_size();
	return true;
}

static void ReplaceFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &haystack_vector = args.data[0];
	const auto &needle_vector = args.data[1];
	const auto &thread_vector = args.data[2];

	const bool constant_needle =
	    needle_vector.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(needle_vector);
	duckdb_re2::LiteralFinder constant_finder;
	if (constant_needle) {
		const auto &needle = ConstantVector::GetData<string_t>(needle_vector)[0];
		constant_finder = duckdb_re2::LiteralFinder(absl::string_view(needle.GetData(), needle.GetSize()));
	}
	vector<char> buffer;
	auto &heap = StringVector::GetStringHeap(result);
	TernaryExecutor::Execute<string_t, string_t, string_t, string_t>(
	    haystack_vector, needle_vector, thread_vector, result,
	    [&](string_t input_string, string_t needle_string, string_t thread_string) {
		    const auto size_needle = needle_string.GetSize();
		    if (size_needle == 0 || input_string.GetSize() < size_needle) {
			    return input_string;
		    }
		    const auto finder =
		        constant_needle ? constant_finder
		                        : duckdb_re2::LiteralFinder(absl::string_view(needle_string.GetData(), size_needle));
		    if (!ReplaceScalarFunction(state, input_string, needle_string, thread_string, finder, buffer)) {
			    return input_string;
		    }
		    return heap.AddString(buffer.data(), buffer.size());
	    });
	StringVector::AddHeapReference(result, haystack_vector);
}

ScalarFunction ReplaceFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::VARCHAR, ReplaceFunction);
	fun.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("source", LogicalType::VARCHAR)
	    .AddParameter("target", LogicalType::VARCHAR);
	return fun;
}

} // namespace duckdb
