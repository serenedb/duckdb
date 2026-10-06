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

#include "duckdb/common/types/vector.hpp"

namespace duckdb {

class BoundOperatorExpression;

//! Evaluates IN / NOT IN over a short list of constants with one typed pass over the input, keeping SQL NULL
//! semantics. Returns false when the expression has another shape and the generic evaluation must run.
struct ConstantInList {
	static constexpr idx_t MAX_CONSTANTS = 32;
	static bool Supports(const LogicalType &type);
	static bool TryExecute(const BoundOperatorExpression &expr, Vector &left, idx_t count, Vector &result);
};

} // namespace duckdb
