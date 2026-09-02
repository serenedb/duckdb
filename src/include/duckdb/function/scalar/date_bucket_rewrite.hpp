//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/scalar/date_bucket_rewrite.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

class ClientContext;

struct DateBucketSpec {
	bool calendar = false;
	int64_t width = 1;
	int64_t anchor = 0;

	int64_t Bucket(int64_t micros) const;
};

class DateBucketRewrite : public BucketRewrite {
public:
	DateBucketRewrite(ClientContext &context, DateBucketSpec spec, idx_t input_index, LogicalType input_type,
	                  LogicalType result_type, bool anno_domini_only);

	idx_t InputIndex() const override;
	bool TryBucketRange(const BaseStatistics &input_stats, int64_t &min_bucket, int64_t &max_bucket) const override;
	unique_ptr<Expression> Bucket(unique_ptr<Expression> input) const override;
	unique_ptr<Expression> Unbucket(unique_ptr<Expression> bucket) const override;

private:
	ClientContext &context;
	DateBucketSpec spec;
	idx_t input_index;
	LogicalType input_type;
	LogicalType result_type;
	bool anno_domini_only;
};

unique_ptr<BucketRewrite> DateTruncBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr);
unique_ptr<BucketRewrite> TimeBucketBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr);

} // namespace duckdb
