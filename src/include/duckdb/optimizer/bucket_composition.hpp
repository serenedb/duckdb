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

#include <absl/algorithm/container.h>

#include "duckdb/function/scalar/date_bucket_rewrite.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression.hpp"

#include <initializer_list>
#include <string_view>

namespace duckdb {

class ClientContext;

inline bool NameIn(std::string_view name, std::initializer_list<std::string_view> names) {
	return absl::c_linear_search(names, name);
}

bool TryFoldConstant(ClientContext &context, const Expression &expr, Value &value);
unique_ptr<Expression> &BucketRewriteInput(Expression &group, idx_t index);
unique_ptr<BucketRewrite> GetHookedBucketRewrite(ClientContext &context, const Expression &group);
vector<unique_ptr<BucketRewrite>> CompositeBucketRewrites(ClientContext &context, Expression &group);
vector<unique_ptr<BucketRewrite>> CoordinateBucketRewrites(ClientContext &context,
                                                           const vector<reference<Expression>> &groups);

} // namespace duckdb
