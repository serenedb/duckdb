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

#include "duckdb/planner/expression.hpp"

namespace duckdb {

//! ILIKE is lower(x) LIKE lower(pattern), so lower(x) = c, for a constant c that is ASCII, already lowercase and
//! free of LIKE wildcards and backslashes, is x ILIKE c (and <> is NOT ILIKE). Returns nullptr for other shapes.
struct LowerEqualityToILike {
	static unique_ptr<Expression> TryRewrite(const Expression &expr);
};

} // namespace duckdb
