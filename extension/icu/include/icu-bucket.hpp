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

#include "duckdb/common/unique_ptr.hpp"

namespace duckdb {

class BoundCastExpression;
class BoundFunctionExpression;
class BucketRewrite;
class ClientContext;
class ExtensionLoader;

unique_ptr<BucketRewrite> ICUDateTruncBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr);
unique_ptr<BucketRewrite> ICUTimeBucketBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr);
unique_ptr<BucketRewrite> ICUDateCastBucketRewrite(ClientContext &context, const BoundCastExpression &cast);
unique_ptr<BucketRewrite> ICUStrfTimeBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr);
unique_ptr<BucketRewrite> ICUMonthNameBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr);
unique_ptr<BucketRewrite> ICUDayNameBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr);

void RegisterICUBucketFunctions(ExtensionLoader &loader);

} // namespace duckdb
