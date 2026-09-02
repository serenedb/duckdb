#include "duckdb/common/operator/date_trunc_operators.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar/date_functions.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

namespace duckdb {

namespace {

bool TryGetConstant(const Expression &expr, int64_t &value) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	const auto &constant = expr.Cast<BoundConstantExpression>().GetValue();
	if (constant.IsNull()) {
		return false;
	}
	value = constant.GetValue<int64_t>();
	return true;
}

int64_t Bucket(timestamp_t ts, int64_t width, int64_t anchor) {
	return DateTrunc::FloorDiv(ts.value - anchor, width);
}

timestamp_t Unbucket(int64_t bucket, int64_t width, int64_t anchor) {
	return timestamp_t(bucket * width + anchor);
}

int64_t MonthBucket(timestamp_t ts, int64_t width, int64_t anchor) {
	return DateTrunc::FloorDiv(DateTrunc::MonthIndex(ts) - anchor, width);
}

timestamp_t MonthUnbucket(int64_t bucket, int64_t width, int64_t anchor) {
	return DateTrunc::MonthIndexStart(bucket * width + anchor);
}

bool TryGetConstants(DataChunk &args, int64_t &width, int64_t &anchor) {
	auto &width_vector = args.data[1];
	auto &anchor_vector = args.data[2];
	if (width_vector.GetVectorType() != VectorType::CONSTANT_VECTOR ||
	    anchor_vector.GetVectorType() != VectorType::CONSTANT_VECTOR || ConstantVector::IsNull(width_vector) ||
	    ConstantVector::IsNull(anchor_vector)) {
		return false;
	}
	width = *ConstantVector::GetData<int64_t>(width_vector);
	anchor = *ConstantVector::GetData<int64_t>(anchor_vector);
	return true;
}

template <class INPUT, class RESULT, RESULT (*FUN)(INPUT, int64_t, int64_t)>
void Execute(DataChunk &args, Vector &result) {
	int64_t width = 0;
	int64_t anchor = 0;
	if (TryGetConstants(args, width, anchor)) {
		UnaryExecutor::Execute<INPUT, RESULT>(args.data[0], result, args.size(),
		                                      [&](INPUT input) { return FUN(input, width, anchor); });
		return;
	}
	TernaryExecutor::Execute<INPUT, int64_t, int64_t, RESULT>(args.data[0], args.data[1], args.data[2], result,
	                                                          args.size(), FUN);
}

void BucketFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	Execute<timestamp_t, int64_t, Bucket>(args, result);
}

void UnbucketFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	Execute<int64_t, timestamp_t, Unbucket>(args, result);
}

void MonthBucketFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	Execute<timestamp_t, int64_t, MonthBucket>(args, result);
}

void MonthUnbucketFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	Execute<int64_t, timestamp_t, MonthUnbucket>(args, result);
}

template <class INPUT, class FUN>
unique_ptr<BaseStatistics> RangeStatistics(FunctionStatisticsInput &input, const LogicalType &result_type, FUN fun) {
	auto &children = input.expr.GetChildren();
	int64_t width = 0;
	int64_t anchor = 0;
	if (!TryGetConstant(*children[1], width) || !TryGetConstant(*children[2], anchor) || width <= 0) {
		return nullptr;
	}
	auto &child = input.child_stats[0];
	if (!NumericStats::HasMinMax(child)) {
		return nullptr;
	}
	const auto min = NumericStats::GetMin<INPUT>(child);
	const auto max = NumericStats::GetMax<INPUT>(child);
	if (min > max || !Value::IsFinite(min) || !Value::IsFinite(max)) {
		return nullptr;
	}
	auto result = NumericStats::CreateEmpty(result_type);
	result.CopyBase(child);
	NumericStats::SetMin(result, Value::CreateValue(fun(min, width, anchor)));
	NumericStats::SetMax(result, Value::CreateValue(fun(max, width, anchor)));
	return result.ToUnique();
}

unique_ptr<BaseStatistics> BucketStatistics(ClientContext &context, FunctionStatisticsInput &input) {
	return RangeStatistics<timestamp_t>(input, LogicalType::BIGINT, Bucket);
}

unique_ptr<BaseStatistics> UnbucketStatistics(ClientContext &context, FunctionStatisticsInput &input) {
	return RangeStatistics<int64_t>(input, LogicalType::TIMESTAMP, Unbucket);
}

unique_ptr<BaseStatistics> MonthBucketStatistics(ClientContext &context, FunctionStatisticsInput &input) {
	return RangeStatistics<timestamp_t>(input, LogicalType::BIGINT, MonthBucket);
}

unique_ptr<BaseStatistics> MonthUnbucketStatistics(ClientContext &context, FunctionStatisticsInput &input) {
	return RangeStatistics<int64_t>(input, LogicalType::TIMESTAMP, MonthUnbucket);
}

} // namespace

ScalarFunction InternalDateTruncBucketFun::GetFunction() {
	return ScalarFunction(Identifier(Name), {LogicalType::TIMESTAMP, LogicalType::BIGINT, LogicalType::BIGINT},
	                      LogicalType::BIGINT, BucketFunction, nullptr, BucketStatistics);
}

ScalarFunction InternalDateTruncUnbucketFun::GetFunction() {
	return ScalarFunction(Identifier(Name), {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT},
	                      LogicalType::TIMESTAMP, UnbucketFunction, nullptr, UnbucketStatistics);
}

ScalarFunction InternalDateTruncMonthBucketFun::GetFunction() {
	return ScalarFunction(Identifier(Name), {LogicalType::TIMESTAMP, LogicalType::BIGINT, LogicalType::BIGINT},
	                      LogicalType::BIGINT, MonthBucketFunction, nullptr, MonthBucketStatistics);
}

ScalarFunction InternalDateTruncMonthUnbucketFun::GetFunction() {
	return ScalarFunction(Identifier(Name), {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT},
	                      LogicalType::TIMESTAMP, MonthUnbucketFunction, nullptr, MonthUnbucketStatistics);
}

} // namespace duckdb
