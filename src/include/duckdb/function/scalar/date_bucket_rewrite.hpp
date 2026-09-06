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

class GranularBucketRewrite : public BucketRewrite {
public:
	virtual int64_t GranularityMicros() const = 0;
	virtual bool Contains(const GranularBucketRewrite &finer) const = 0;
	virtual optional_ptr<const GranularBucketRewrite> Core() const {
		return this;
	}
	virtual unique_ptr<BucketRewrite> TryTimeOfDay(ClientContext &context, Expression &input) const {
		return nullptr;
	}

	static bool Nested(int64_t width, int64_t anchor, int64_t finer_width, int64_t finer_anchor) {
		return finer_width > 0 && width % finer_width == 0 && (anchor - finer_anchor) % finer_width == 0;
	}
	static optional_ptr<const GranularBucketRewrite> CoreOf(const BucketRewrite &rewrite) {
		auto granular = dynamic_cast<const GranularBucketRewrite *>(&rewrite);
		return granular ? granular->Core() : nullptr;
	}
};

class DelegatingBucketRewrite : public GranularBucketRewrite {
public:
	explicit DelegatingBucketRewrite(unique_ptr<BucketRewrite> inner_p) : inner(std::move(inner_p)) {
	}

	int64_t GranularityMicros() const override {
		return 0;
	}
	optional_ptr<const GranularBucketRewrite> Core() const override {
		return CoreOf(*inner);
	}
	bool Contains(const GranularBucketRewrite &finer) const override {
		auto core = Core();
		return core && core->Contains(finer);
	}
	bool TryBucketRange(const BaseStatistics &input_stats, int64_t &min_bucket, int64_t &max_bucket) const override {
		return inner->TryBucketRange(input_stats, min_bucket, max_bucket);
	}
	unique_ptr<Expression> Bucket(unique_ptr<Expression> input) const override {
		return inner->Bucket(std::move(input));
	}

protected:
	unique_ptr<BucketRewrite> inner;
};

class DateBucketRewrite : public GranularBucketRewrite {
public:
	DateBucketRewrite(ClientContext &context, DateBucketSpec spec, idx_t input_index, LogicalType input_type,
	                  LogicalType result_type, bool anno_domini_only);

	idx_t InputIndex() const override;
	int64_t GranularityMicros() const override;
	bool Contains(const GranularBucketRewrite &finer) const override;
	bool TryBucketRange(const BaseStatistics &input_stats, int64_t &min_bucket, int64_t &max_bucket) const override;
	unique_ptr<Expression> Bucket(unique_ptr<Expression> input) const override;
	unique_ptr<Expression> Unbucket(unique_ptr<Expression> bucket) const override;
	unique_ptr<BucketRewrite> TryTimeOfDay(ClientContext &context, Expression &input) const override;
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

class FunctionBucketRewrite : public DelegatingBucketRewrite {
public:
	FunctionBucketRewrite(unique_ptr<BucketRewrite> inner, const BoundFunctionExpression &expr, idx_t input_index);

	idx_t InputIndex() const override;
	unique_ptr<Expression> Unbucket(unique_ptr<Expression> bucket) const override;

private:
	BoundScalarFunction function;
	vector<unique_ptr<Expression>> arguments;
	unique_ptr<FunctionData> bind_info;
	idx_t input_index;
};

class TimeOfDayBucketRewrite : public BucketRewrite {
public:
	TimeOfDayBucketRewrite(ClientContext &context, ScalarFunction bucket_function, unique_ptr<FunctionData> bind_info,
	                       idx_t input_index, int64_t width, unique_ptr<Expression> shell,
	                       unique_ptr<Expression> template_input, optional_ptr<Expression> custom_input);

	idx_t InputIndex() const override;
	optional_ptr<Expression> CustomInput() const override;
	bool TryBucketRange(const BaseStatistics &input_stats, int64_t &min_bucket, int64_t &max_bucket) const override;
	unique_ptr<Expression> Bucket(unique_ptr<Expression> input) const override;
	unique_ptr<Expression> Unbucket(unique_ptr<Expression> bucket) const override;

private:
	ClientContext &context;
	ScalarFunction bucket_function;
	unique_ptr<FunctionData> bind_info;
	idx_t input_index;
	int64_t width;
	unique_ptr<Expression> shell;
	unique_ptr<Expression> template_input;
	optional_ptr<Expression> custom_input;
};

unique_ptr<BucketRewrite> TimeOfDayRewrite(ClientContext &context, ScalarFunction bucket_function,
                                           unique_ptr<FunctionData> bind_info, idx_t input_index, int64_t width,
                                           const Value &format, optional_ptr<Expression> custom_input = nullptr);
bool TryGetTimeOfDayWidth(const string &format, int64_t &width);
bool TimeOfDayGrid(int64_t width, int64_t anchor);

class CyclicBucketRewrite : public BucketRewrite {
public:
	CyclicBucketRewrite(ScalarFunction unbucket_function, int64_t min_bucket, int64_t max_bucket);

	idx_t InputIndex() const override;
	bool TryConstantRange(int64_t &min_bucket, int64_t &max_bucket) const override;
	bool TryBucketRange(const BaseStatistics &input_stats, int64_t &min_bucket, int64_t &max_bucket) const override;
	unique_ptr<Expression> Unbucket(unique_ptr<Expression> bucket) const override;

protected:
	ScalarFunction unbucket_function;
	int64_t min_bucket;
	int64_t max_bucket;
};

struct DateCoordinates {
	enum class Level : uint8_t { NONE, MILLENNIUM, CENTURY, DECADE, YEAR, QUARTER, MONTH, WEEK, DAY, HOUR, MINUTE, SECOND };

	Level finest = Level::NONE;
	bool year = false;
	bool iso_year = false;
	bool quarter = false;
	bool month = false;
	bool iso_week = false;
	bool day = false;
	bool day_of_year = false;
	bool weekday = false;
	bool hour24 = false;
	bool hour12 = false;
	bool am_pm = false;
	bool minute = false;
	bool second = false;
	bool two_digit_year = false;

	bool AddFormat(const string &format);
	bool AddPart(DatePartSpecifier part);
	bool TryResolve(bool sub_day_constant, DatePartSpecifier &part) const;
	bool TimeOfDayOnly() const;

private:
	void Raise(Level level);
};

unique_ptr<Expression> MakeBucketCall(const ScalarFunction &function, vector<unique_ptr<Expression>> arguments,
                                      unique_ptr<FunctionData> bind_info = nullptr);
unique_ptr<Expression> RebuildShell(ClientContext &context, const Expression &shell, const Expression &template_input,
                                    unique_ptr<Expression> value);
const char *DateTruncPartName(DatePartSpecifier part);
bool TryGetStrfTimeGranularity(const string &format, bool sub_day_constant, DatePartSpecifier &part,
                               bool &two_digit_year);
bool TryGetMicrosRange(const BaseStatistics &stats, int64_t &min, int64_t &max, bool &zoned);

unique_ptr<BucketRewrite> DateTruncBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr);
unique_ptr<BucketRewrite> TimeBucketBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr);
unique_ptr<BucketRewrite> StrfTimeBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr);
unique_ptr<BucketRewrite> MonthNameBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr);
unique_ptr<BucketRewrite> DayNameBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr);
unique_ptr<BucketRewrite> LastDayBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr);
unique_ptr<BucketRewrite> DateBinBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr);

} // namespace duckdb
