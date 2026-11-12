#include "core_functions/scalar/string_functions.hpp"
#include "core_functions/scalar/fast_reverse.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "utf8proc_wrapper.hpp"

#include <string.h>

namespace duckdb {

struct ReverseOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, StringHeap &heap) {
		auto input_data = input.GetData();
		auto input_length = input.GetSize();

		auto target = heap.EmptyString(input_length);
		auto target_data = target.GetDataWriteable();
		FastReverse::Reverse(input_data, input_length, target_data);
		target.Finalize();
		return target;
	}
};

static void ReverseFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::ExecuteString<string_t, string_t, ReverseOperator>(args.data[0], result);
}

ScalarFunction ReverseFun::GetFunction() {
	ScalarFunction fun("reverse", {}, LogicalType::VARCHAR, ReverseFunction);
	fun.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	return fun;
}

} // namespace duckdb
