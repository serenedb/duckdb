////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2026 SereneDB GmbH, Berlin, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is SereneDB GmbH, Berlin, Germany
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/operator/date_trunc_operators.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

struct DateTruncFast {
	template <class TA, class TR, class OP>
	struct FiniteOperator {
		template <class INPUT_TYPE, class RESULT_TYPE>
		static inline RESULT_TYPE Operation(INPUT_TYPE input) {
			if (DUCKDB_UNLIKELY(!Value::IsFinite(input))) {
				return Cast::template Operation<INPUT_TYPE, RESULT_TYPE>(input);
			}
			return OP::template Operation<INPUT_TYPE, RESULT_TYPE>(input);
		}
	};

	template <class TA, class TR, class OP>
	static void Function(DataChunk &args, ExpressionState &state, Vector &result) {
		UnaryExecutor::Execute<TA, TR, FiniteOperator<TA, TR, OP>>(args.data[1], result, args.size());
	}

	template <class TA, class TR>
	static scalar_function_t Callback(DatePartSpecifier part) {
		return DateTrunc::Dispatch(part, [](auto op) -> scalar_function_t { return Function<TA, TR, decltype(op)>; });
	}
};

} // namespace duckdb
