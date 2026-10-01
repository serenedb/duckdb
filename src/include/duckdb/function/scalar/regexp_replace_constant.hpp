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

#include "duckdb/function/scalar/regexp.hpp"

namespace duckdb {

//! First-match regexp_replace with a constant pattern and a constant replacement: the replacement is validated
//! once (on the first non-NULL row, as the per-row path does), rows without a match are returned as-is without a
//! copy, and a match is rewritten into a reused buffer instead of copying the whole input.
struct RegexpReplaceConstant {
	static void Execute(const Vector &strings, string_t replace, RegexLocalState &lstate, Vector &result);
};

} // namespace duckdb
