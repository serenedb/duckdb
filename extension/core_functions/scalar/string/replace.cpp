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

static bool FindNeedles(ExpressionState &state, const string_t &haystack, const string_t &needle,
                        const duckdb_re2::LiteralFinder &finder, vector<idx_t> &positions) {
	auto input_haystack = const_uchar_ptr_cast(haystack.GetData());
	auto size_haystack = haystack.GetSize();
	auto input_needle = const_uchar_ptr_cast(needle.GetData());
	auto size_needle = needle.GetSize();
	positions.clear();
	idx_t pos = 0;
	idx_t unchecked = 0;
	while (size_haystack - pos >= size_needle) {
		auto found = FindStrInStr(input_haystack + pos, size_haystack - pos, input_needle, size_needle, finder);
		if (found == DConstants::INVALID_INDEX) {
			break;
		}
		positions.push_back(pos + found);
		pos += found + size_needle;
		unchecked += found + size_needle;
		if (unchecked >= REPLACE_CHECK_BYTES) {
			state.InterruptCheck();
			unchecked = 0;
		}
	}
	return !positions.empty();
}

static string_t WriteReplaced(const string_t &haystack, const string_t &needle, const string_t &thread,
                              const vector<idx_t> &positions, Vector &result) {
	auto haystack_data = haystack.GetData();
	auto haystack_size = haystack.GetSize();
	auto needle_size = needle.GetSize();
	auto thread_data = thread.GetData();
	auto thread_size = thread.GetSize();
	auto result_size = haystack_size - positions.size() * needle_size + positions.size() * thread_size;
	if (result_size > string_t::MAX_STRING_SIZE) {
		throw OutOfRangeException(
		    "Cannot create a string of size: '%d' in REPLACE, the maximum supported string size is: '%d'", result_size,
		    string_t::MAX_STRING_SIZE);
	}
	auto target = StringVector::EmptyString(result, result_size);
	auto out = target.GetDataWriteable();
	idx_t copied = 0;
	for (auto position : positions) {
		memcpy(out, haystack_data + copied, position - copied);
		out += position - copied;
		memcpy(out, thread_data, thread_size);
		out += thread_size;
		copied = position + needle_size;
	}
	memcpy(out, haystack_data + copied, haystack_size - copied);
	target.Finalize();
	return target;
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
	vector<idx_t> positions;
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
		    if (!FindNeedles(state, input_string, needle_string, finder, positions)) {
			    return input_string;
		    }
		    return WriteReplaced(input_string, needle_string, thread_string, positions, result);
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
