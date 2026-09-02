//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/scalar/date_bucket_rewrite.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/date_part_specifier.hpp"
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
	void RequireYearSpanBelow(int64_t years) {
		max_year_span = years;
	}

private:
	ClientContext &context;
	DateBucketSpec spec;
	idx_t input_index;
	LogicalType input_type;
	LogicalType result_type;
	bool anno_domini_only;
	int64_t max_year_span = 0;
};

class FunctionBucketRewrite : public BucketRewrite {
public:
	FunctionBucketRewrite(unique_ptr<BucketRewrite> inner, const BoundFunctionExpression &expr, idx_t input_index);

	idx_t InputIndex() const override;
	bool TryBucketRange(const BaseStatistics &input_stats, int64_t &min_bucket, int64_t &max_bucket) const override;
	unique_ptr<Expression> Bucket(unique_ptr<Expression> input) const override;
	unique_ptr<Expression> Unbucket(unique_ptr<Expression> bucket) const override;

private:
	unique_ptr<BucketRewrite> inner;
	BoundScalarFunction function;
	vector<unique_ptr<Expression>> arguments;
	unique_ptr<FunctionData> bind_info;
	idx_t input_index;
};

class CyclicBucketRewrite : public BucketRewrite {
public:
	CyclicBucketRewrite(ScalarFunction unbucket_function, int64_t min_bucket, int64_t max_bucket);

	idx_t InputIndex() const override;
	bool TryBucketRange(const BaseStatistics &input_stats, int64_t &min_bucket, int64_t &max_bucket) const override;
	unique_ptr<Expression> Unbucket(unique_ptr<Expression> bucket) const override;

protected:
	ScalarFunction unbucket_function;
	int64_t min_bucket;
	int64_t max_bucket;
};

bool TryGetStrfTimeGranularity(const string &format, bool sub_day_constant, DatePartSpecifier &part,
                               bool &two_digit_year);
bool TryGetMicrosRange(const BaseStatistics &stats, int64_t &min, int64_t &max, bool &zoned);

unique_ptr<BucketRewrite> DateTruncBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr);
unique_ptr<BucketRewrite> TimeBucketBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr);
unique_ptr<BucketRewrite> StrfTimeBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr);
unique_ptr<BucketRewrite> MonthNameBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr);
unique_ptr<BucketRewrite> DayNameBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr);
unique_ptr<BucketRewrite> LastDayBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr);

} // namespace duckdb
