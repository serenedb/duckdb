#include "core_functions/scalar/blob_functions.hpp"
#include "duckdb/common/types/blob.hpp"

namespace duckdb {

namespace {
struct Base64EncodeOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, StringHeap &heap) {
		auto result_str = heap.EmptyString(Blob::ToBase64Size(input));
		Blob::ToBase64(input, result_str.GetDataWriteable());
		result_str.Finalize();
		return result_str;
	}
};

struct Base64DecodeOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, StringHeap &heap) {
		// FromBase64Size is an upper bound (simdutf ignores ASCII whitespace); the exact decoded length
		// is returned by FromBase64, so re-wrap the buffer with the bytes actually written.
		auto result_blob = heap.EmptyString(Blob::FromBase64Size(input));
		auto decoded_size =
		    Blob::FromBase64(input, data_ptr_cast(result_blob.GetDataWriteable()), result_blob.GetSize());
		return string_t(result_blob.GetDataWriteable(), UnsafeNumericCast<uint32_t>(decoded_size));
	}
};

void Base64EncodeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	// decode is also a nop cast, but requires verification if the provided string is actually
	UnaryExecutor::ExecuteString<string_t, string_t, Base64EncodeOperator>(args.data[0], result);
}

void Base64DecodeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	// decode is also a nop cast, but requires verification if the provided string is actually
	UnaryExecutor::ExecuteString<string_t, string_t, Base64DecodeOperator>(args.data[0], result);
}

} // namespace

ScalarFunction ToBase64Fun::GetFunction() {
	return ScalarFunction({LogicalType::BLOB}, LogicalType::VARCHAR, Base64EncodeFunction);
}

ScalarFunction FromBase64Fun::GetFunction() {
	ScalarFunction function({LogicalType::VARCHAR}, LogicalType::BLOB, Base64DecodeFunction);
	function.SetFallible();
	return function;
}

} // namespace duckdb
