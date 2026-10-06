#include "core_functions/scalar/string_functions.hpp"
#include "core_functions/scalar/fast_replace.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"

#include <string.h>
#include <ctype.h>
#include <unordered_map>

namespace duckdb {

ScalarFunction ReplaceFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::VARCHAR, FastReplace::Execute);
	fun.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("source", LogicalType::VARCHAR)
	    .AddParameter("target", LogicalType::VARCHAR);
	return fun;
}

} // namespace duckdb
