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

#include "duckdb/optimizer/bucket_composition.hpp"

#include "duckdb/common/types/interval.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar/date_bucket_rewrite.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

#include <cmath>

namespace duckdb {

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

int64_t Gcd(int64_t a, int64_t b) {
	a = AbsValue(a);
	b = AbsValue(b);
	while (b) {
		const auto r = a % b;
		a = b;
		b = r;
	}
	return a;
}

struct Folder {
	explicit Folder(ClientContext &context_p) : context(context_p) {
	}

	bool IsConstant(const Expression &expr) const {
		return expr.IsFoldable();
	}

	bool TryValue(const Expression &expr, Value &value) const {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			value = expr.Cast<BoundConstantExpression>().GetValue();
			return true;
		}
		return expr.IsFoldable() && ExpressionExecutor::TryEvaluateScalar(context, expr, value);
	}

	bool IsNonNullConstant(const Expression &expr) const {
		Value value;
		return IsConstant(expr) && TryValue(expr, value) && !value.IsNull();
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

bool TryGetInteger(const Value &value, int64_t &result) {
	if (value.IsNull() || !value.type().IsIntegral()) {
		return false;
	}
	Value casted = value;
	if (!casted.DefaultTryCastAs(LogicalType::BIGINT)) {
		return false;
	}
	result = casted.GetValue<int64_t>();
	return true;
}

const string &FunctionName(const Expression &expr) {
	return expr.Cast<BoundFunctionExpression>().Function().GetName().GetIdentifierName();
}

bool NameIn(const string &name, std::initializer_list<const char *> names) {
	for (auto candidate : names) {
		if (name == candidate) {
			return true;
		}
	}
	return false;
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
		auto result = ReplaceColumn(expr->Copy(), input);
		if (type.id() == LogicalTypeId::BIGINT) {
			return result;
		}
		return BoundCastExpression::AddCastToType(context, std::move(result), LogicalType::BIGINT);
	}
	unique_ptr<Expression> Unbucket(unique_ptr<Expression> bucket) const override {
		if (type.id() == LogicalTypeId::BIGINT) {
			return bucket;
		}
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
		Value casted = value;
		if (!casted.DefaultTryCastAs(LogicalType::BIGINT)) {
			return false;
		}
		result = casted.GetValue<int64_t>();
		return AbsValue(result) <= SMALL_INTEGER_LIMIT;
	}

	ClientContext &context;
	unique_ptr<Expression> expr;
	Expression &column;
	LogicalType type;
	bool signed_zero;
};

bool IsMonotone(const Folder &folder, const Expression &expr, optional_ptr<Expression> &column, idx_t &columns) {
	if (folder.IsConstant(expr)) {
		return true;
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF:
		columns++;
		column = const_cast<Expression *>(&expr);
		return expr.GetReturnType().IsNumeric();
	case ExpressionClass::BOUND_CAST: {
		auto &cast = expr.Cast<BoundCastExpression>();
		return !cast.IsTryCast() && cast.GetReturnType().IsNumeric() && cast.Child().GetReturnType().IsNumeric() &&
		       IsMonotone(folder, cast.Child(), column, columns);
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &children = expr.Cast<BoundFunctionExpression>().GetChildren();
		const auto &name = FunctionName(expr);
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

struct Leaf {
	unique_ptr<BucketRewrite> rewrite;
	optional_ptr<Expression> input;
	LeafInfo info;
};

bool TryLeaf(const Folder &folder, Expression &expr, Leaf &leaf) {
	if (auto hooked = GetHookedBucketRewrite(folder.context, expr)) {
		leaf.input = BucketRewriteInput(expr, hooked->InputIndex()).get();
		if (auto granular = dynamic_cast<GranularBucketRewrite *>(hooked.get())) {
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

bool NumericStep(const string &name, idx_t child_index, const Value &constant, const LogicalType &result_type,
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
		info.multiple_of = integral_constant ? Gcd(info.multiple_of, integer) : 1;
	} else if (name == "*") {
		if (factor == 0) {
			return false;
		}
		info.scale *= std::fabs(factor);
		info.offset *= std::fabs(factor);
		info.multiple_of = integral_constant ? info.multiple_of * AbsValue(integer) : 1;
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

bool WrapperAllowed(const Folder &folder, const Expression &expr, idx_t child_index, LeafInfo &info) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		return CastAllowed(expr.Cast<BoundCastExpression>(), info);
	}
	auto &children = expr.Cast<BoundFunctionExpression>().GetChildren();
	const auto &name = FunctionName(expr);
	const auto &child_type = children[child_index]->GetReturnType();
	if (NameIn(name, {"+", "-", "*", "/", "//"}) && children.size() == 2) {
		Value constant;
		if (!folder.TryValue(*children[1 - child_index], constant)) {
			return false;
		}
		if (info.kind == LeafInfo::Kind::NUMERIC) {
			return expr.GetReturnType().IsNumeric() &&
			       NumericStep(name, child_index, constant, expr.GetReturnType(), info);
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
			info.granularity = Gcd(info.granularity, shift);
		}
		if (expr.GetReturnType().id() == LogicalTypeId::DATE) {
			info.granularity = Interval::MICROS_PER_DAY;
		}
		return true;
	}
	if (NameIn(name, {"||", "concat"})) {
		info.Other();
		return true;
	}
	if (NameIn(name, {"epoch", "epoch_ms", "epoch_us", "epoch_ns"}) && children.size() == 1) {
		if (info.kind != LeafInfo::Kind::DATE || info.granularity <= 0) {
			return false;
		}
		const int64_t unit = name == "epoch" ? Interval::MICROS_PER_SEC : name == "epoch_ms" ? Interval::MICROS_PER_MSEC : 1;
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
	return false;
}

class CompositeBucketRewrite : public BucketRewrite {
public:
	CompositeBucketRewrite(unique_ptr<BucketRewrite> inner_p, Expression &input_p, const Expression &group,
	                       vector<idx_t> path_p, double value_limit_p)
	    : inner(std::move(inner_p)), input(input_p), shell(group.Copy()), path(std::move(path_p)),
	      value_limit(value_limit_p) {
	}

	idx_t InputIndex() const override {
		return 0;
	}
	optional_ptr<Expression> CustomInput() const override {
		return &input;
	}
	bool TryBucketRange(const BaseStatistics &stats, int64_t &min_bucket, int64_t &max_bucket) const override {
		if (!inner->TryBucketRange(stats, min_bucket, max_bucket)) {
			return false;
		}
		return double(MaxValue(AbsValue(min_bucket), AbsValue(max_bucket))) <= value_limit;
	}
	unique_ptr<Expression> Bucket(unique_ptr<Expression> input_p) const override {
		return inner->Bucket(std::move(input_p));
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
	unique_ptr<BucketRewrite> inner;
	Expression &input;
	unique_ptr<Expression> shell;
	vector<idx_t> path;
	double value_limit;
};

bool Descend(const Folder &folder, Expression &expr, vector<idx_t> &path, Leaf &leaf, idx_t depth) {
	if (TryLeaf(folder, expr, leaf)) {
		return true;
	}
	if (depth >= MAX_WRAPPER_DEPTH) {
		return false;
	}
	idx_t index = 0;
	auto child = SingleNonConstantChild(folder, expr, index);
	if (!child) {
		return false;
	}
	path.push_back(index);
	return Descend(folder, *child, path, leaf, depth + 1) && WrapperAllowed(folder, expr, index, leaf.info);
}

} // namespace

vector<unique_ptr<BucketRewrite>> CompositeBucketRewrites(ClientContext &context, Expression &group) {
	vector<unique_ptr<BucketRewrite>> result;
	Folder folder(context);
	if (auto numeric = TryNumericLeaf(folder, group)) {
		result.push_back(std::move(numeric));
	}
	vector<idx_t> path;
	Leaf leaf;
	idx_t index = 0;
	auto child = SingleNonConstantChild(folder, group, index);
	if (!child) {
		return result;
	}
	path.push_back(index);
	if (Descend(folder, *child, path, leaf, 1) && WrapperAllowed(folder, group, index, leaf.info)) {
		result.push_back(make_uniq<CompositeBucketRewrite>(std::move(leaf.rewrite), *leaf.input, group,
		                                                   std::move(path), leaf.info.ValueLimit()));
	}
	return result;
}

} // namespace duckdb
