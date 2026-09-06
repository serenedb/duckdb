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

#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/optimizer/bucket_composition.hpp"

#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/date_bucket_rewrite.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

#include <cmath>
#include <numeric>

namespace duckdb {

bool TryFoldConstant(ClientContext &context, const Expression &expr, Value &value) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		value = expr.Cast<BoundConstantExpression>().GetValue();
		return true;
	}
	return expr.IsFoldable() && ExpressionExecutor::TryEvaluateScalar(context, expr, value);
}

unique_ptr<Expression> &BucketRewriteInput(Expression &group, idx_t index) {
	if (group.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		return group.Cast<BoundCastExpression>().ChildMutable();
	}
	return group.Cast<BoundFunctionExpression>().GetChildrenMutable()[index];
}

unique_ptr<BucketRewrite> GetHookedBucketRewrite(ClientContext &context, const Expression &group) {
	switch (group.GetExpressionClass()) {
	case ExpressionClass::BOUND_FUNCTION: {
		auto &function = group.Cast<BoundFunctionExpression>();
		if (!function.Function().HasBucketRewriteCallback()) {
			return nullptr;
		}
		return function.Function().GetBucketRewriteCallback()(context, function);
	}
	case ExpressionClass::BOUND_CAST: {
		auto &cast = group.Cast<BoundCastExpression>();
		if (!cast.GetBoundCast().bucket_rewrite) {
			return nullptr;
		}
		return cast.GetBoundCast().bucket_rewrite(context, cast);
	}
	default:
		return nullptr;
	}
}

namespace {

constexpr int64_t SMALL_INTEGER_LIMIT = int64_t(1) << 52;
constexpr double EXACT_DOUBLE_LIMIT = double(int64_t(1) << 53);
constexpr double EXACT_FLOAT_LIMIT = double(int64_t(1) << 24);
constexpr double EPOCH_SECONDS_LIMIT = 1e13;
constexpr double EPOCH_MILLIS_LIMIT = 1e16;
constexpr idx_t MAX_WRAPPER_DEPTH = 8;

struct LeafInfo {
	enum class Kind : uint8_t { NUMERIC, DATE, OTHER };
	Kind kind = Kind::OTHER;
	int64_t granularity = 0;
	int64_t multiple_of = 1;
	bool dynamic = false;
	double magnitude = 0;
	double scale = 1;
	double offset = 0;
	double exact_limit = EXACT_DOUBLE_LIMIT;
	bool floating = false;

	void Numeric(int64_t multiple, double bound, bool dynamic_p) {
		kind = Kind::NUMERIC;
		multiple_of = multiple;
		magnitude = bound;
		dynamic = dynamic_p;
		scale = 1;
		offset = 0;
	}
	void Other() {
		kind = Kind::OTHER;
	}
	bool Representable(double limit) {
		exact_limit = MinValue(exact_limit, limit);
		floating = true;
		return dynamic || magnitude * scale + offset <= exact_limit;
	}
	double ValueLimit() const {
		if (!floating || !dynamic) {
			return NumericLimits<double>::Maximum();
		}
		return (exact_limit - offset) / scale;
	}
};

struct Folder {
	explicit Folder(ClientContext &context_p) : context(context_p) {
	}

	bool IsConstant(const Expression &expr) const {
		return expr.IsFoldable();
	}

	bool TryValue(const Expression &expr, Value &value) const {
		return TryFoldConstant(context, expr, value);
	}

	bool IsNonNullConstant(const Expression &expr) const {
		Value value;
		return TryValue(expr, value) && !value.IsNull();
	}

	ClientContext &context;
};

bool TryGetDouble(const Value &value, double &result) {
	if (value.IsNull() || !value.type().IsNumeric()) {
		return false;
	}
	Value casted = value;
	if (!casted.DefaultTryCastAs(LogicalType::DOUBLE)) {
		return false;
	}
	result = casted.GetValue<double>();
	return std::isfinite(result);
}

bool TryCastToInteger(const Value &value, int64_t &result) {
	Value casted = value;
	if (!casted.DefaultTryCastAs(LogicalType::BIGINT)) {
		return false;
	}
	result = casted.GetValue<int64_t>();
	return true;
}

bool TryGetInteger(const Value &value, int64_t &result) {
	return !value.IsNull() && value.type().IsIntegral() && TryCastToInteger(value, result);
}

std::string_view FunctionName(const Expression &expr) {
	return expr.Cast<BoundFunctionExpression>().Function().GetName().GetIdentifierName();
}

unique_ptr<Expression> ReplaceColumn(unique_ptr<Expression> expr, unique_ptr<Expression> &replacement) {
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		return std::move(replacement);
	}
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		if (replacement) {
			child = ReplaceColumn(std::move(child), replacement);
		}
	});
	return expr;
}

class NumericBucketRewrite : public BucketRewrite {
public:
	NumericBucketRewrite(ClientContext &context_p, const Expression &expr_p, Expression &column_p)
	    : context(context_p), expr(expr_p.Copy()), column(column_p), type(expr_p.GetReturnType()),
	      signed_zero(type.IsFloating() && expr_p.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	                  NameIn(FunctionName(expr_p), {"ceil", "ceiling", "trunc", "round"})) {
	}

	idx_t InputIndex() const override {
		return 0;
	}
	optional_ptr<Expression> CustomInput() const override {
		return &column;
	}
	bool TryBucketRange(const BaseStatistics &stats, int64_t &min_bucket, int64_t &max_bucket) const override {
		if (stats.GetStatsType() != StatisticsType::NUMERIC_STATS || !NumericStats::HasMinMax(stats)) {
			return false;
		}
		const auto min = NumericStats::Min(stats);
		const auto max = NumericStats::Max(stats);
		int64_t lo = 0;
		int64_t hi = 0;
		if (!FiniteBound(min) || !FiniteBound(max) || !TryEvaluateAt(min, lo) || !TryEvaluateAt(max, hi)) {
			return false;
		}
		min_bucket = MinValue(lo, hi) - 1;
		max_bucket = MaxValue(lo, hi) + 1;
		return !signed_zero || MinValue(lo, hi) > 0 || MaxValue(lo, hi) < 0;
	}
	unique_ptr<Expression> Bucket(unique_ptr<Expression> input) const override {
		return BoundCastExpression::AddCastToType(context, ReplaceColumn(expr->Copy(), input), LogicalType::BIGINT);
	}
	unique_ptr<Expression> Unbucket(unique_ptr<Expression> bucket) const override {
		return BoundCastExpression::AddCastToType(context, std::move(bucket), type);
	}

private:
	static bool FiniteBound(const Value &bound) {
		if (bound.IsNull()) {
			return false;
		}
		if (!bound.type().IsFloating()) {
			return true;
		}
		return std::isfinite(bound.GetValue<double>());
	}

	bool TryEvaluateAt(const Value &bound, int64_t &result) const {
		unique_ptr<Expression> constant = make_uniq<BoundConstantExpression>(bound);
		auto evaluated = ReplaceColumn(expr->Copy(), constant);
		Value value;
		if (!evaluated->IsFoldable() || !ExpressionExecutor::TryEvaluateScalar(context, *evaluated, value) ||
		    value.IsNull()) {
			return false;
		}
		return ToBucket(value, result);
	}

	static bool ToBucket(const Value &value, int64_t &result) {
		if (value.type().IsFloating()) {
			const auto d = value.GetValue<double>();
			if (!std::isfinite(d) || std::fabs(d) > double(SMALL_INTEGER_LIMIT) || d != std::floor(d)) {
				return false;
			}
			result = int64_t(d);
			return true;
		}
		return TryCastToInteger(value, result) && AbsValue(result) <= SMALL_INTEGER_LIMIT;
	}

	ClientContext &context;
	unique_ptr<Expression> expr;
	Expression &column;
	LogicalType type;
	bool signed_zero;
};

bool DateTyped(const Expression &expr) {
	const auto &type = expr.GetReturnType();
	return type.id() == LogicalTypeId::DATE || LogicalType::TypeIsTimestamp(type);
}

bool IsEpochBase(const Folder &folder, Expression &expr, optional_ptr<Expression> &column, idx_t &columns) {
	auto &children = expr.Cast<BoundFunctionExpression>().GetChildrenMutable();
	const auto &name = FunctionName(expr);
	optional_ptr<Expression> argument;
	if (children.size() == 1 && NameIn(name, {"epoch", "epoch_ms", "epoch_us", "epoch_ns"})) {
		argument = children[0].get();
	} else if (children.size() == 2 && NameIn(name, {"date_part", "datepart", "extract"})) {
		Value part;
		if (folder.TryValue(*children[0], part) && part.type().id() == LogicalTypeId::VARCHAR &&
		    StringUtil::Lower(StringValue::Get(part)) == "epoch") {
			argument = children[1].get();
		}
	}
	if (!argument || argument->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF || !DateTyped(*argument)) {
		return false;
	}
	columns++;
	column = argument;
	return true;
}

bool IsMonotone(const Folder &folder, Expression &expr, optional_ptr<Expression> &column, idx_t &columns) {
	if (folder.IsConstant(expr)) {
		return true;
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF:
		columns++;
		column = &expr;
		return expr.GetReturnType().IsNumeric();
	case ExpressionClass::BOUND_CAST: {
		auto &cast = expr.Cast<BoundCastExpression>();
		return !cast.IsTryCast() && cast.GetReturnType().IsNumeric() && cast.Child().GetReturnType().IsNumeric() &&
		       IsMonotone(folder, *cast.ChildMutable(), column, columns);
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &children = expr.Cast<BoundFunctionExpression>().GetChildrenMutable();
		const auto &name = FunctionName(expr);
		if (IsEpochBase(folder, expr, column, columns)) {
			return true;
		}
		if (children.size() == 2 && NameIn(name, {"+", "-", "*"})) {
			return (folder.IsConstant(*children[0]) || folder.IsConstant(*children[1])) &&
			       IsMonotone(folder, *children[0], column, columns) && IsMonotone(folder, *children[1], column, columns);
		}
		if (children.size() == 2 && NameIn(name, {"/", "//", "round", "trunc"})) {
			return folder.IsNonNullConstant(*children[1]) && IsMonotone(folder, *children[0], column, columns);
		}
		if (children.size() == 1 &&
		    NameIn(name, {"-", "floor", "ceil", "ceiling", "trunc", "round", "ln", "log", "log2", "log10", "sqrt", "exp",
		                  "cbrt", "sign"})) {
			return IsMonotone(folder, *children[0], column, columns);
		}
		return false;
	}
	default:
		return false;
	}
}

bool IntegerValued(const Folder &folder, const Expression &expr) {
	if (expr.GetReturnType().IsIntegral()) {
		return true;
	}
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &children = expr.Cast<BoundFunctionExpression>().GetChildren();
	const auto &name = FunctionName(expr);
	if (NameIn(name, {"floor", "ceil", "ceiling", "trunc", "sign"})) {
		return true;
	}
	if (name == "round") {
		if (children.size() == 1) {
			return true;
		}
		Value scale;
		int64_t digits = 0;
		return folder.TryValue(*children[1], scale) && TryGetInteger(scale, digits) && digits <= 0;
	}
	return false;
}

unique_ptr<NumericBucketRewrite> TryNumericLeaf(const Folder &folder, Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF || !expr.GetReturnType().IsNumeric() ||
	    !IntegerValued(folder, expr)) {
		return nullptr;
	}
	optional_ptr<Expression> column;
	idx_t columns = 0;
	if (!IsMonotone(folder, expr, column, columns) || columns != 1) {
		return nullptr;
	}
	return make_uniq<NumericBucketRewrite>(folder.context, expr, *column);
}

class CaseBucketRewrite : public BucketRewrite {
public:
	CaseBucketRewrite(Expression &group_p, vector<Value> values_p, vector<int64_t> branches_p, int64_t else_branch_p)
	    : group(group_p), type(group_p.GetReturnType()), values(std::move(values_p)), branches(std::move(branches_p)),
	      else_branch(else_branch_p) {
	}

	idx_t InputIndex() const override {
		return 0;
	}
	optional_ptr<Expression> CustomInput() const override {
		return &group;
	}
	bool TryConstantRange(int64_t &min_bucket, int64_t &max_bucket) const override {
		min_bucket = 0;
		max_bucket = NumericCast<int64_t>(values.size()) - 1;
		return true;
	}
	bool TryBucketRange(const BaseStatistics &stats, int64_t &min_bucket, int64_t &max_bucket) const override {
		return TryConstantRange(min_bucket, max_bucket);
	}
	unique_ptr<Expression> Bucket(unique_ptr<Expression> input) const override {
		auto &original = input->Cast<BoundCaseExpression>();
		auto result = make_uniq<BoundCaseExpression>(LogicalType::BIGINT);
		idx_t index = 0;
		for (auto &check : original.CaseChecksMutable()) {
			BoundCaseCheck bucketed;
			bucketed.when_expr = std::move(check.when_expr);
			bucketed.then_expr = Index(branches[index++]);
			result->CaseChecksMutable().push_back(std::move(bucketed));
		}
		result->ElseMutable() = Index(else_branch);
		return std::move(result);
	}
	unique_ptr<Expression> Unbucket(unique_ptr<Expression> bucket) const override {
		auto result = make_uniq<BoundCaseExpression>(type);
		for (idx_t i = 0; i < values.size(); i++) {
			BoundCaseCheck check;
			check.when_expr = BoundComparisonExpression::Create(
			    ExpressionType::COMPARE_EQUAL, bucket->Copy(),
			    make_uniq<BoundConstantExpression>(Value::BIGINT(NumericCast<int64_t>(i))));
			check.then_expr = make_uniq<BoundConstantExpression>(values[i]);
			result->CaseChecksMutable().push_back(std::move(check));
		}
		result->ElseMutable() = make_uniq<BoundConstantExpression>(Value(type));
		return std::move(result);
	}

private:
	static unique_ptr<Expression> Index(int64_t branch) {
		return make_uniq<BoundConstantExpression>(branch < 0 ? Value(LogicalType::BIGINT) : Value::BIGINT(branch));
	}

	Expression &group;
	LogicalType type;
	vector<Value> values;
	vector<int64_t> branches;
	int64_t else_branch;
};

unique_ptr<CaseBucketRewrite> TryCaseLeaf(const Folder &folder, Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CASE || expr.IsVolatile()) {
		return nullptr;
	}
	auto &kase = expr.Cast<BoundCaseExpression>();
	vector<Value> values;
	vector<int64_t> branches;
	auto branch_of = [&](const Expression &result, int64_t &branch) {
		Value value;
		if (!folder.TryValue(result, value)) {
			return false;
		}
		if (value.IsNull()) {
			branch = -1;
			return true;
		}
		for (idx_t i = 0; i < values.size(); i++) {
			if (values[i] == value) {
				branch = NumericCast<int64_t>(i);
				return true;
			}
		}
		branch = NumericCast<int64_t>(values.size());
		values.push_back(value);
		return true;
	};
	for (auto &check : kase.CaseChecks()) {
		int64_t branch = 0;
		if (!branch_of(*check.then_expr, branch)) {
			return nullptr;
		}
		branches.push_back(branch);
	}
	int64_t else_branch = 0;
	if (branches.empty() || !branch_of(kase.Else(), else_branch) || values.empty()) {
		return nullptr;
	}
	return make_uniq<CaseBucketRewrite>(expr, std::move(values), std::move(branches), else_branch);
}

struct Leaf {
	unique_ptr<BucketRewrite> rewrite;
	optional_ptr<Expression> input;
	LeafInfo info;
};

bool TryLeaf(const Folder &folder, Expression &expr, Leaf &leaf) {
	if (auto hooked = GetHookedBucketRewrite(folder.context, expr)) {
		leaf.input = BucketRewriteInput(expr, hooked->InputIndex()).get();
		auto granular = dynamic_cast<GranularBucketRewrite *>(hooked.get());
		if (granular && granular->GranularityMicros() > 0) {
			leaf.info.kind = LeafInfo::Kind::DATE;
			leaf.info.granularity = granular->GranularityMicros();
		}
		if (expr.GetReturnType().id() == LogicalTypeId::DATE) {
			leaf.info.kind = LeafInfo::Kind::DATE;
			leaf.info.granularity = Interval::MICROS_PER_DAY;
		}
		leaf.rewrite = std::move(hooked);
		return true;
	}
	if (auto numeric = TryNumericLeaf(folder, expr)) {
		leaf.input = numeric->CustomInput();
		leaf.info.Numeric(1, double(SMALL_INTEGER_LIMIT), true);
		leaf.rewrite = std::move(numeric);
		return true;
	}
	if (auto kase = TryCaseLeaf(folder, expr)) {
		leaf.input = kase->CustomInput();
		leaf.info.Other();
		leaf.rewrite = std::move(kase);
		return true;
	}
	return false;
}

optional_ptr<Expression> SingleNonConstantChild(const Folder &folder, Expression &expr, idx_t &index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		index = 0;
		return expr.Cast<BoundCastExpression>().ChildMutable().get();
	}
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return nullptr;
	}
	optional_ptr<Expression> found;
	auto &children = expr.Cast<BoundFunctionExpression>().GetChildrenMutable();
	for (idx_t i = 0; i < children.size(); i++) {
		if (folder.IsNonNullConstant(*children[i])) {
			continue;
		}
		if (found) {
			return nullptr;
		}
		found = children[i].get();
		index = i;
	}
	return found;
}

bool DayAligned(const LeafInfo &info) {
	return info.kind == LeafInfo::Kind::DATE && info.granularity > 0 &&
	       info.granularity % Interval::MICROS_PER_DAY == 0;
}

bool CastAllowed(const BoundCastExpression &cast, LeafInfo &info) {
	if (cast.IsTryCast()) {
		return false;
	}
	const auto source = cast.Child().GetReturnType().id();
	const auto &target = cast.GetReturnType();
	if (target.id() == LogicalTypeId::VARCHAR) {
		info.Other();
		return true;
	}
	if (source == LogicalTypeId::DATE) {
		switch (target.id()) {
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::TIMESTAMP_TZ:
		case LogicalTypeId::TIMESTAMP_NS:
		case LogicalTypeId::TIMESTAMP_MS:
		case LogicalTypeId::TIMESTAMP_SEC:
			return true;
		default:
			return false;
		}
	}
	if (info.kind == LeafInfo::Kind::NUMERIC && cast.Child().GetReturnType().IsNumeric() && target.IsNumeric()) {
		if (target.id() == LogicalTypeId::FLOAT) {
			return info.Representable(EXACT_FLOAT_LIMIT);
		}
		if (target.IsFloating()) {
			return info.Representable(EXACT_DOUBLE_LIMIT);
		}
		return true;
	}
	if (info.kind != LeafInfo::Kind::DATE || info.granularity <= 0) {
		return false;
	}
	const bool zoned_source = source == LogicalTypeId::TIMESTAMP_TZ;
	if (source != LogicalTypeId::TIMESTAMP && !zoned_source) {
		return false;
	}
	switch (target.id()) {
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		return DayAligned(info) || (!zoned_source && target.id() == LogicalTypeId::TIMESTAMP);
	case LogicalTypeId::TIMESTAMP_NS:
		info.Other();
		return !zoned_source;
	case LogicalTypeId::TIMESTAMP_MS:
		info.Other();
		return !zoned_source && info.granularity % Interval::MICROS_PER_MSEC == 0;
	case LogicalTypeId::TIMESTAMP_SEC:
		info.Other();
		return !zoned_source && info.granularity % Interval::MICROS_PER_SEC == 0;
	default:
		return false;
	}
}

bool IntervalShift(const Value &value, int64_t &micros) {
	if (value.type().id() != LogicalTypeId::INTERVAL) {
		return false;
	}
	const auto interval = value.GetValue<interval_t>();
	if (interval.months != 0) {
		return false;
	}
	micros = int64_t(interval.days) * Interval::MICROS_PER_DAY + interval.micros;
	return true;
}

bool NumericStep(std::string_view name, idx_t child_index, const Value &constant, const LogicalType &result_type,
                 LeafInfo &info) {
	double factor = 0;
	int64_t integer = 0;
	if (!TryGetDouble(constant, factor)) {
		return false;
	}
	const bool integral_constant = TryGetInteger(constant, integer);
	if (name == "+" || name == "-") {
		if (name == "-" && child_index == 1) {
			return false;
		}
		info.offset += std::fabs(factor);
		info.multiple_of = integral_constant ? std::gcd(info.multiple_of, integer) : 1;
	} else if (name == "*") {
		if (factor == 0) {
			return false;
		}
		info.scale *= std::fabs(factor);
		info.offset *= std::fabs(factor);
		int64_t multiple = 0;
		if (!integral_constant || integer == NumericLimits<int64_t>::Minimum() ||
		    !TryMultiplyOperator::Operation(info.multiple_of, AbsValue(integer), multiple)) {
			multiple = 1;
		}
		info.multiple_of = multiple;
	} else if (name == "/" || name == "//") {
		if (child_index != 0 || factor == 0) {
			return false;
		}
		if (result_type.IsIntegral()) {
			if (!integral_constant || info.multiple_of % integer != 0) {
				return false;
			}
			info.multiple_of /= AbsValue(integer);
		} else {
			info.multiple_of = 1;
		}
		info.scale /= std::fabs(factor);
		info.offset /= std::fabs(factor);
	} else {
		return false;
	}
	if (result_type.IsFloating()) {
		return info.Representable(result_type.id() == LogicalTypeId::FLOAT ? EXACT_FLOAT_LIMIT : EXACT_DOUBLE_LIMIT);
	}
	return true;
}

bool ArithmeticWrapper(const Folder &folder, const Expression &expr, std::string_view name, idx_t child_index,
                       const LogicalType &child_type, LeafInfo &info) {
	auto &children = expr.Cast<BoundFunctionExpression>().GetChildren();
	Value constant;
	if (!folder.TryValue(*children[1 - child_index], constant)) {
		return false;
	}
	if (info.kind == LeafInfo::Kind::NUMERIC) {
		return expr.GetReturnType().IsNumeric() && NumericStep(name, child_index, constant, expr.GetReturnType(), info);
	}
	if (info.kind != LeafInfo::Kind::DATE || child_index != 0 || !NameIn(name, {"+", "-"})) {
		return false;
	}
	int64_t shift = 0;
	int64_t days = 0;
	if (child_type.id() == LogicalTypeId::DATE && TryGetInteger(constant, days)) {
		shift = days * Interval::MICROS_PER_DAY;
	} else if (!IntervalShift(constant, shift)) {
		return false;
	}
	if (shift != 0) {
		info.granularity = std::gcd(info.granularity, shift);
	}
	if (expr.GetReturnType().id() == LogicalTypeId::DATE) {
		info.granularity = Interval::MICROS_PER_DAY;
	}
	return true;
}

bool EpochInverseWrapper(std::string_view name, LeafInfo &info) {
	if (info.kind != LeafInfo::Kind::NUMERIC) {
		return false;
	}
	const double unit = name == "to_timestamp" ? 1e6 : name == "epoch_ms" ? 1e3 : 1;
	const double spacing = (info.dynamic ? info.scale : double(info.multiple_of)) * unit;
	if (spacing < 1) {
		return false;
	}
	const double shift = (info.dynamic ? info.offset : 0) * unit;
	info.kind = LeafInfo::Kind::DATE;
	info.granularity = 0;
	if (spacing == std::floor(spacing) && shift == std::floor(shift) && spacing < 1e18 && shift < 1e18) {
		info.granularity = shift == 0 ? int64_t(spacing) : std::gcd(int64_t(spacing), int64_t(shift));
	}
	return true;
}

bool EpochWrapper(std::string_view name, LeafInfo &info) {
	if (info.kind != LeafInfo::Kind::DATE || info.granularity <= 0) {
		return false;
	}
	const int64_t unit =
	    name == "epoch" ? Interval::MICROS_PER_SEC : name == "epoch_ms" ? Interval::MICROS_PER_MSEC : 1;
	if (info.granularity % unit != 0) {
		return false;
	}
	if (name == "epoch") {
		info.Numeric(info.granularity / unit, EPOCH_SECONDS_LIMIT, false);
	} else if (name == "epoch_ms") {
		info.Numeric(info.granularity / unit, EPOCH_MILLIS_LIMIT, false);
	} else {
		info.Other();
	}
	return true;
}

bool WrapperAllowed(const Folder &folder, const Expression &expr, idx_t child_index, LeafInfo &info) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		return CastAllowed(expr.Cast<BoundCastExpression>(), info);
	}
	auto &children = expr.Cast<BoundFunctionExpression>().GetChildren();
	const auto &name = FunctionName(expr);
	const auto &child_type = children[child_index]->GetReturnType();
	if (NameIn(name, {"+", "-", "*", "/", "//"}) && children.size() == 2) {
		return ArithmeticWrapper(folder, expr, name, child_index, child_type, info);
	}
	if (NameIn(name, {"||", "concat"})) {
		info.Other();
		return true;
	}
	if (NameIn(name, {"to_timestamp", "make_timestamp", "make_timestamptz", "epoch_ms"}) && children.size() == 1 &&
	    child_type.IsNumeric()) {
		return EpochInverseWrapper(name, info);
	}
	if (NameIn(name, {"epoch", "epoch_ms", "epoch_us", "epoch_ns"}) && children.size() == 1) {
		return EpochWrapper(name, info);
	}
	return false;
}

class WrappedBucketRewrite : public DelegatingBucketRewrite {
public:
	WrappedBucketRewrite(unique_ptr<BucketRewrite> inner_p, Expression &input_p)
	    : DelegatingBucketRewrite(std::move(inner_p)), input(input_p) {
	}

	idx_t InputIndex() const override {
		return 0;
	}
	optional_ptr<Expression> CustomInput() const override {
		return &input;
	}

protected:
	Expression &input;
};

class CompositeBucketRewrite : public WrappedBucketRewrite {
public:
	CompositeBucketRewrite(unique_ptr<BucketRewrite> inner_p, Expression &input_p, const Expression &group,
	                       vector<idx_t> path_p, double value_limit_p)
	    : WrappedBucketRewrite(std::move(inner_p), input_p), shell(group.Copy()), path(std::move(path_p)),
	      value_limit(value_limit_p) {
	}

	bool TryConstantRange(int64_t &min_bucket, int64_t &max_bucket) const override {
		return inner->TryConstantRange(min_bucket, max_bucket);
	}
	bool TryBucketRange(const BaseStatistics &stats, int64_t &min_bucket, int64_t &max_bucket) const override {
		if (!WrappedBucketRewrite::TryBucketRange(stats, min_bucket, max_bucket)) {
			return false;
		}
		return double(MaxValue(AbsValue(min_bucket), AbsValue(max_bucket))) <= value_limit;
	}
	unique_ptr<Expression> Unbucket(unique_ptr<Expression> bucket) const override {
		auto result = shell->Copy();
		reference<unique_ptr<Expression>> node(result);
		for (const auto index : path) {
			node = BucketRewriteInput(*node.get(), index);
		}
		node.get() = inner->Unbucket(std::move(bucket));
		return result;
	}

private:
	unique_ptr<Expression> shell;
	vector<idx_t> path;
	double value_limit;
};

struct LabelPart {
	enum class Kind : uint8_t { CONSTANT, NUMERIC, ALPHA, MIXED };
	Kind kind = Kind::CONSTANT;
	string text;
	optional_ptr<Expression> input;
};

Expression &StripVarcharCasts(Expression &expr) {
	reference<Expression> current(expr);
	while (current.get().GetExpressionClass() == ExpressionClass::BOUND_CAST &&
	       current.get().GetReturnType().id() == LogicalTypeId::VARCHAR &&
	       !current.get().Cast<BoundCastExpression>().IsTryCast()) {
		current = *current.get().Cast<BoundCastExpression>().ChildMutable();
	}
	return current.get();
}

bool IsConcat(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION && NameIn(FunctionName(expr), {"||", "concat"});
}

void FlattenConcat(Expression &expr, vector<reference<Expression>> &parts) {
	if (IsConcat(expr)) {
		for (auto &child : expr.Cast<BoundFunctionExpression>().GetChildrenMutable()) {
			FlattenConcat(*child, parts);
		}
		return;
	}
	parts.push_back(expr);
}

bool DateInput(const Expression &expr) {
	switch (expr.GetReturnType().id()) {
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		return true;
	default:
		return false;
	}
}

bool PartFunctionSpecifier(std::string_view name, DatePartSpecifier &part) {
	if (name == "dayofyear") {
		part = DatePartSpecifier::DOY;
		return true;
	}
	if (name == "dayofweek") {
		part = DatePartSpecifier::DOW;
		return true;
	}
	if (name == "weekofyear") {
		part = DatePartSpecifier::WEEK;
		return true;
	}
	if (NameIn(name, {"year", "quarter", "month", "week", "day", "hour", "minute", "second", "decade", "century",
	                  "millennium", "isoyear", "isodow", "yearweek"})) {
		return TryGetDatePartSpecifier(string(name), part);
	}
	return false;
}

bool ClassifyLabelPart(const Folder &folder, Expression &part_expr, DateCoordinates &coordinates, LabelPart &out) {
	auto &expr = StripVarcharCasts(part_expr);
	Value value;
	if (folder.IsNonNullConstant(expr) && folder.TryValue(expr, value)) {
		Value text = value;
		if (!text.DefaultTryCastAs(LogicalType::VARCHAR)) {
			return false;
		}
		out.kind = LabelPart::Kind::CONSTANT;
		out.text = StringValue::Get(text);
		return true;
	}
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &children = expr.Cast<BoundFunctionExpression>().GetChildrenMutable();
	const auto &name = FunctionName(expr);
	DatePartSpecifier part;
	if (name == "strftime" && children.size() == 2) {
		const idx_t constant_index = folder.IsNonNullConstant(*children[0]) ? 0 : 1;
		Value format;
		if (!folder.TryValue(*children[constant_index], format) || format.type().id() != LogicalTypeId::VARCHAR ||
		    !coordinates.AddFormat(StringValue::Get(format))) {
			return false;
		}
		out.kind = LabelPart::Kind::MIXED;
		out.input = children[1 - constant_index].get();
	} else if (NameIn(name, {"monthname", "dayname"}) && children.size() == 1) {
		coordinates.AddPart(name == "monthname" ? DatePartSpecifier::MONTH : DatePartSpecifier::DOW);
		out.kind = LabelPart::Kind::ALPHA;
		out.input = children[0].get();
	} else if (NameIn(name, {"date_part", "datepart", "extract"}) && children.size() == 2) {
		Value part_name;
		if (!folder.TryValue(*children[0], part_name) || part_name.type().id() != LogicalTypeId::VARCHAR ||
		    !TryGetDatePartSpecifier(StringValue::Get(part_name), part) || !coordinates.AddPart(part)) {
			return false;
		}
		out.kind = LabelPart::Kind::NUMERIC;
		out.input = children[1].get();
	} else if (children.size() == 1 && PartFunctionSpecifier(name, part)) {
		if (!coordinates.AddPart(part)) {
			return false;
		}
		out.kind = LabelPart::Kind::NUMERIC;
		out.input = children[0].get();
	} else {
		return false;
	}
	return DateInput(*out.input);
}

bool SeparatorFits(const LabelPart &separator, const LabelPart &leaf, bool leaf_before) {
	if (separator.text.empty()) {
		return false;
	}
	const char boundary = leaf_before ? separator.text.front() : separator.text.back();
	switch (leaf.kind) {
	case LabelPart::Kind::NUMERIC:
		return !StringUtil::CharacterIsDigit(boundary);
	case LabelPart::Kind::ALPHA:
		return !StringUtil::CharacterIsAlpha(boundary);
	default:
		return !StringUtil::CharacterIsAlphaNumeric(boundary);
	}
}

class DateLabelRewrite : public WrappedBucketRewrite {
public:
	DateLabelRewrite(ClientContext &context_p, unique_ptr<BucketRewrite> inner_p, Expression &input_p,
	                 const Expression &group)
	    : WrappedBucketRewrite(std::move(inner_p), input_p), context(context_p), template_input(input_p.Copy()),
	      shell(group.Copy()) {
	}

	unique_ptr<Expression> Unbucket(unique_ptr<Expression> bucket) const override {
		return RebuildShell(context, *shell, *template_input, inner->Unbucket(std::move(bucket)));
	}

private:
	ClientContext &context;
	unique_ptr<Expression> template_input;
	unique_ptr<Expression> shell;
};

unique_ptr<BucketRewrite> CoordinateBucketRewrite(ClientContext &context, Expression &group, Expression &input,
                                                  DatePartSpecifier period);

unique_ptr<BucketRewrite> TryDateLabel(const Folder &folder, Expression &group) {
	if (!IsConcat(group) && group.GetReturnType().id() != LogicalTypeId::VARCHAR) {
		return nullptr;
	}
	vector<reference<Expression>> flat;
	FlattenConcat(group, flat);
	vector<LabelPart> parts;
	DateCoordinates coordinates;
	optional_ptr<Expression> input;
	for (auto &part_expr : flat) {
		LabelPart part;
		if (!ClassifyLabelPart(folder, part_expr.get(), coordinates, part)) {
			return nullptr;
		}
		if (part.input) {
			if (input && !input->Equals(*part.input)) {
				return nullptr;
			}
			input = part.input;
		}
		parts.push_back(std::move(part));
	}
	if (!input || coordinates.two_digit_year) {
		return nullptr;
	}
	for (idx_t i = 0; i + 1 < parts.size(); i++) {
		const bool leaf = parts[i].kind != LabelPart::Kind::CONSTANT;
		const bool next_leaf = parts[i + 1].kind != LabelPart::Kind::CONSTANT;
		if (leaf && next_leaf) {
			return nullptr;
		}
		if (leaf && i + 2 < parts.size() && parts[i + 2].kind != LabelPart::Kind::CONSTANT &&
		    (!SeparatorFits(parts[i + 1], parts[i], true) || !SeparatorFits(parts[i + 1], parts[i + 2], false))) {
			return nullptr;
		}
	}
	DatePartSpecifier part;
	if (!coordinates.TryResolve(input->GetReturnType().id() == LogicalTypeId::DATE, part)) {
		return nullptr;
	}
	return CoordinateBucketRewrite(folder.context, group, *input, part);
}

bool DatePartGroup(ClientContext &context, Expression &group, DateCoordinates &coordinates,
                   optional_ptr<Expression> &input) {
	Folder folder(context);
	LabelPart part;
	if (!ClassifyLabelPart(folder, group, coordinates, part) || part.kind == LabelPart::Kind::CONSTANT || !part.input) {
		return false;
	}
	input = part.input;
	return true;
}

unique_ptr<BucketRewrite> CoordinateBucketRewrite(ClientContext &context, Expression &group, Expression &input,
                                                  DatePartSpecifier period) {
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(make_uniq<BoundConstantExpression>(Value(DateTruncPartName(period))));
	arguments.push_back(input.Copy());
	FunctionBinder binder(context);
	ErrorData error;
	auto truncation = binder.BindScalarFunction(Identifier(DEFAULT_SCHEMA), Identifier("date_trunc"),
	                                            std::move(arguments), error);
	if (!truncation || truncation->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return nullptr;
	}
	auto inner = GetHookedBucketRewrite(context, *truncation);
	if (!inner) {
		return nullptr;
	}
	return make_uniq<DateLabelRewrite>(context, std::move(inner), input, group);
}

unique_ptr<BucketRewrite> TryTimeOfDayCast(const Folder &folder, Expression &group) {
	if (group.GetExpressionClass() != ExpressionClass::BOUND_CAST) {
		return nullptr;
	}
	auto &cast = group.Cast<BoundCastExpression>();
	if (cast.IsTryCast() || cast.GetReturnType().id() != LogicalTypeId::TIME) {
		return nullptr;
	}
	auto &inner = *cast.ChildMutable();
	auto inner_rewrite = GetHookedBucketRewrite(folder.context, inner);
	if (!inner_rewrite) {
		return nullptr;
	}
	auto core = GranularBucketRewrite::CoreOf(*inner_rewrite);
	if (!core) {
		return nullptr;
	}
	auto &input = *BucketRewriteInput(inner, inner_rewrite->InputIndex());
	return core->TryTimeOfDay(folder.context, input);
}

struct Candidate {
	Leaf leaf;
	vector<idx_t> path;
};

void CollectLeaves(const Folder &folder, Expression &expr, vector<idx_t> &path, idx_t depth, vector<Candidate> &out) {
	Leaf leaf;
	if (TryLeaf(folder, expr, leaf)) {
		Candidate candidate;
		candidate.leaf = std::move(leaf);
		candidate.path = path;
		out.push_back(std::move(candidate));
	}
	if (depth >= MAX_WRAPPER_DEPTH) {
		return;
	}
	idx_t index = 0;
	auto child = SingleNonConstantChild(folder, expr, index);
	if (!child) {
		return;
	}
	path.push_back(index);
	const auto before = out.size();
	CollectLeaves(folder, *child, path, depth + 1, out);
	path.pop_back();
	for (idx_t i = before; i < out.size();) {
		if (WrapperAllowed(folder, expr, index, out[i].leaf.info)) {
			i++;
		} else {
			out.erase(out.begin() + NumericCast<int64_t>(i));
		}
	}
}

} // namespace

vector<unique_ptr<BucketRewrite>> CompositeBucketRewrites(ClientContext &context, Expression &group) {
	vector<unique_ptr<BucketRewrite>> result;
	Folder folder(context);
	if (auto numeric = TryNumericLeaf(folder, group)) {
		result.push_back(std::move(numeric));
	}
	if (auto kase = TryCaseLeaf(folder, group)) {
		result.push_back(std::move(kase));
	}
	idx_t index = 0;
	auto child = SingleNonConstantChild(folder, group, index);
	if (child) {
		vector<idx_t> path;
		path.push_back(index);
		vector<Candidate> candidates;
		CollectLeaves(folder, *child, path, 1, candidates);
		for (auto &candidate : candidates) {
			if (WrapperAllowed(folder, group, index, candidate.leaf.info)) {
				result.push_back(make_uniq<CompositeBucketRewrite>(std::move(candidate.leaf.rewrite),
				                                                   *candidate.leaf.input, group,
				                                                   std::move(candidate.path),
				                                                   candidate.leaf.info.ValueLimit()));
			}
		}
	}
	if (auto label = TryDateLabel(folder, group)) {
		result.push_back(std::move(label));
	}
	if (auto time_cast = TryTimeOfDayCast(folder, group)) {
		result.push_back(std::move(time_cast));
	}
	return result;
}

vector<unique_ptr<BucketRewrite>> CoordinateBucketRewrites(ClientContext &context,
                                                           const vector<reference<Expression>> &groups) {
	struct CoordinateSet {
		optional_ptr<Expression> input;
		DateCoordinates coordinates;
		vector<idx_t> members;
	};
	vector<CoordinateSet> sets;
	for (idx_t group_idx = 0; group_idx < groups.size(); group_idx++) {
		DateCoordinates probe;
		optional_ptr<Expression> input;
		if (!DatePartGroup(context, groups[group_idx], probe, input)) {
			continue;
		}
		optional_ptr<CoordinateSet> set;
		for (auto &candidate : sets) {
			if (candidate.input->Equals(*input)) {
				set = &candidate;
				break;
			}
		}
		if (!set) {
			sets.emplace_back();
			set = &sets.back();
			set->input = input;
		}
		if (DatePartGroup(context, groups[group_idx], set->coordinates, input)) {
			set->members.push_back(group_idx);
		}
	}
	vector<unique_ptr<BucketRewrite>> rewrites(groups.size());
	for (auto &set : sets) {
		DatePartSpecifier period;
		if (set.members.size() < 2 || set.coordinates.two_digit_year ||
		    set.input->GetReturnType().id() == LogicalTypeId::DATE ||
		    !set.coordinates.TryResolve(false, period)) {
			continue;
		}
		for (auto member : set.members) {
			rewrites[member] = CoordinateBucketRewrite(context, groups[member], *set.input, period);
		}
	}
	return rewrites;
}

} // namespace duckdb
