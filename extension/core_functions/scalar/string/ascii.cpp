#include "core_functions/scalar/string_functions.hpp"
#include "utf8proc.hpp"
#include "utf8proc_wrapper.hpp"

namespace duckdb {

struct AsciiOperator {
	template <class TA, class TR>
	static inline TR Operation(const TA &input) {
		auto str = input.GetData();
		int utf8_bytes = 4;
		return Utf8Proc::UTF8ToCodepoint(str, utf8_bytes, input.GetSize());
	}
};

ScalarFunction ASCIIFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::INTEGER, ScalarFunction::UnaryFunction<string_t, int32_t, AsciiOperator>);
	fun.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	return fun;
}

} // namespace duckdb
