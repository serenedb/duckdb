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

#include "duckdb/optimizer/rule/monotone_predicate.hpp"

#include <absl/algorithm/container.h>

#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector_cache.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/optimizer/bucket_composition.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

#include <cmath>
#include <cstring>

namespace duckdb {

MonotonePredicateRule::zone_transitions_t MonotonePredicateRule::zone_transitions = nullptr;

namespace {

constexpr idx_t SEARCH_WIDTH = 254;
constexpr int64_t TRANSITION_MARGIN = 2 * Interval::MICROS_PER_DAY;

struct Domain {
	LogicalType type;
	bool valid = false;
	bool has_infinity = false;
	bool floating = false;
	int64_t min_key = 0;
	int64_t max_key = 0;

	static int64_t DoubleKey(double value) {
		int64_t bits;
		memcpy(&bits, &value, sizeof(bits));
		return bits < 0 ? bits ^ NumericLimits<int64_t>::Maximum() : bits;
	}

	static double KeyDouble(int64_t key) {
		const int64_t bits = key < 0 ? key ^ NumericLimits<int64_t>::Maximum() : key;
		double value;
		memcpy(&value, &bits, sizeof(value));
		return value;
	}

	static Domain For(const LogicalType &type) {
		Domain domain;
		domain.type = type;
		switch (type.id()) {
		case LogicalTypeId::TINYINT:
		case LogicalTypeId::SMALLINT:
		case LogicalTypeId::INTEGER:
		case LogicalTypeId::BIGINT:
		case LogicalTypeId::UTINYINT:
		case LogicalTypeId::USMALLINT:
		case LogicalTypeId::UINTEGER:
		case LogicalTypeId::DATE:
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::TIMESTAMP_SEC:
		case LogicalTypeId::TIMESTAMP_MS:
		case LogicalTypeId::TIMESTAMP_NS:
		case LogicalTypeId::TIMESTAMP_TZ:
			domain.valid = true;
			break;
		case LogicalTypeId::DOUBLE:
			domain.valid = true;
			domain.floating = true;
			break;
		default:
			return domain;
		}
		domain.has_infinity = type.IsTemporal() || domain.floating;
		domain.min_key = domain.Key(Value::MinimumValue(type));
		domain.max_key = domain.Key(Value::MaximumValue(type));
		return domain;
	}

	int64_t Key(const Value &value) const {
		if (floating) {
			return DoubleKey(value.GetValueUnsafe<double>());
		}
		return Hugeint::Cast<int64_t>(IntegralValue::Get(value));
	}

	Value ToValue(int64_t key) const {
		return floating ? Value::DOUBLE(KeyDouble(key)) : Value::Numeric(type, key);
	}
};

enum class InfinityRule : uint8_t { PRESERVED, NULLED };

struct Chain {
	reference<Expression> root;
	reference<Expression> input;
	vector<idx_t> path;
	InfinityRule infinity = InfinityRule::PRESERVED;
	bool folds = false;

	explicit Chain(Expression &expr) : root(expr), input(expr) {
	}
};

struct MonotoneFunction {
	std::string_view name;
	idx_t input_index;
	idx_t min_arguments;
	idx_t max_arguments;
	InfinityRule infinity;
};

const MonotoneFunction MONOTONE_FUNCTIONS[] = {
    {"date_trunc", 1, 2, NumericLimits<idx_t>::Maximum(), InfinityRule::PRESERVED},
    {"datetrunc", 1, 2, NumericLimits<idx_t>::Maximum(), InfinityRule::PRESERVED},
    {"date_bin", 1, 2, NumericLimits<idx_t>::Maximum(), InfinityRule::PRESERVED},
    {"time_bucket", 1, 2, 4, InfinityRule::PRESERVED},
    {"last_day", 0, 1, 1, InfinityRule::NULLED},
    {"epoch", 0, 1, 1, InfinityRule::NULLED},
    {"epoch_ms", 0, 1, 1, InfinityRule::NULLED},
    {"epoch_us", 0, 1, 1, InfinityRule::NULLED},
    {"epoch_ns", 0, 1, 1, InfinityRule::NULLED},
    {"year", 0, 1, 1, InfinityRule::NULLED},
    {"isoyear", 0, 1, 1, InfinityRule::NULLED},
    {"decade", 0, 1, 1, InfinityRule::NULLED},
    {"century", 0, 1, 1, InfinityRule::NULLED},
    {"millennium", 0, 1, 1, InfinityRule::NULLED},
};

bool MonotoneDatePart(const Expression &part) {
	if (part.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	const auto &value = part.Cast<BoundConstantExpression>().GetValue();
	if (value.IsNull() || value.type().id() != LogicalTypeId::VARCHAR) {
		return false;
	}
	DatePartSpecifier specifier;
	if (!TryGetDatePartSpecifier(StringValue::Get(value), specifier)) {
		return false;
	}
	switch (specifier) {
	case DatePartSpecifier::YEAR:
	case DatePartSpecifier::ISOYEAR:
	case DatePartSpecifier::DECADE:
	case DatePartSpecifier::CENTURY:
	case DatePartSpecifier::MILLENNIUM:
	case DatePartSpecifier::EPOCH:
		return true;
	default:
		return false;
	}
}

optional_idx ArithmeticInput(const vector<unique_ptr<Expression>> &children, bool allow_constant_left, bool positive,
                             bool integral_handled_elsewhere) {
	if (children.size() != 2) {
		return optional_idx();
	}
	const idx_t input = children[0]->IsFoldable() ? 1 : 0;
	auto &constant = *children[1 - input];
	if (children[input]->IsFoldable() || !constant.IsFoldable() || (input == 1 && !allow_constant_left) ||
	    constant.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return optional_idx();
	}
	const auto &value = constant.Cast<BoundConstantExpression>().GetValue();
	if (value.IsNull()) {
		return optional_idx();
	}
	const auto &input_type = children[input]->GetReturnType();
	if (integral_handled_elsewhere && input_type.IsIntegral() && value.type().IsIntegral()) {
		return optional_idx();
	}
	if (positive) {
		const bool positive_constant = value.type().IsNumeric() && value > Value::Numeric(value.type(), 0);
		return positive_constant ? optional_idx(input) : optional_idx();
	}
	if (value.type().id() == LogicalTypeId::INTERVAL) {
		const auto interval = value.GetValueUnsafe<interval_t>();
		const bool zoned = input_type.id() == LogicalTypeId::TIMESTAMP_TZ;
		return !zoned || (interval.months == 0 && interval.days == 0) ? optional_idx(input) : optional_idx();
	}
	return value.type().IsNumeric() ? optional_idx(input) : optional_idx();
}

bool CastAllowed(const LogicalType &from, const LogicalType &to) {
	const auto f = from.id();
	const auto t = to.id();
	if (from.IsIntegral() && (to.IsIntegral() || t == LogicalTypeId::DOUBLE)) {
		return GetTypeIdSize(from.InternalType()) < GetTypeIdSize(to.InternalType()) ||
		       t == LogicalTypeId::DOUBLE;
	}
	switch (f) {
	case LogicalTypeId::DATE:
		return t == LogicalTypeId::TIMESTAMP || t == LogicalTypeId::TIMESTAMP_TZ;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
		return t == LogicalTypeId::DATE || t == LogicalTypeId::TIMESTAMP || t == LogicalTypeId::TIMESTAMP_SEC ||
		       t == LogicalTypeId::TIMESTAMP_MS || t == LogicalTypeId::TIMESTAMP_NS;
	case LogicalTypeId::TIMESTAMP_TZ:
		return t == LogicalTypeId::DATE;
	default:
		return false;
	}
}

bool OthersFoldable(const vector<unique_ptr<Expression>> &children, idx_t input_index) {
	for (idx_t i = 0; i < children.size(); i++) {
		if (i != input_index && !children[i]->IsFoldable()) {
			return false;
		}
	}
	return true;
}

optional_idx StepInput(const Expression &expr, InfinityRule &infinity, bool &folds) {
	const auto function_step = [&folds](optional_idx index) {
		folds = folds || index.IsValid();
		return index;
	};
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = expr.Cast<BoundCastExpression>();
		if (cast.IsTryCast() || !CastAllowed(cast.Child().GetReturnType(), cast.GetReturnType())) {
			return optional_idx();
		}
		folds = folds || GetTypeIdSize(cast.GetReturnType().InternalType()) <
		                     GetTypeIdSize(cast.Child().GetReturnType().InternalType());
		return optional_idx(0);
	}
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return optional_idx();
	}
	auto &func = expr.Cast<BoundFunctionExpression>();
	auto &children = func.GetChildren();
	const std::string_view name = func.Function().GetName().GetIdentifierName();
	auto monotone = absl::c_find_if(MONOTONE_FUNCTIONS, [&](const MonotoneFunction &entry) { return name == entry.name; });
	if (monotone != std::end(MONOTONE_FUNCTIONS)) {
		if (children.size() < monotone->min_arguments || children.size() > monotone->max_arguments ||
		    !OthersFoldable(children, monotone->input_index)) {
			return optional_idx();
		}
		infinity = monotone->infinity;
		return function_step(optional_idx(monotone->input_index));
	}
	if (NameIn(name, {"date_part", "datepart"})) {
		if (children.size() != 2 || !MonotoneDatePart(*children[0])) {
			return optional_idx();
		}
		infinity = InfinityRule::NULLED;
		return function_step(optional_idx(1));
	}
	if (NameIn(name, {"floor", "ceil", "ceiling", "trunc", "round"})) {
		if (children.empty() || children.size() > 2 || !OthersFoldable(children, 0)) {
			return optional_idx();
		}
		if (children.size() == 2 && !children[0]->GetReturnType().IsIntegral()) {
			return optional_idx();
		}
		return function_step(optional_idx(0));
	}
	if (name == "+") {
		return function_step(ArithmeticInput(children, true, false, true));
	}
	if (name == "-") {
		return function_step(ArithmeticInput(children, false, false, true));
	}
	if (name == "*") {
		return function_step(ArithmeticInput(children, true, true, true));
	}
	if (NameIn(name, {"/", "//"})) {
		return function_step(ArithmeticInput(children, false, true, false));
	}
	return optional_idx();
}

bool Analyse(Expression &expr, Chain &chain) {
	reference<Expression> current(expr);
	while (true) {
		InfinityRule infinity = InfinityRule::PRESERVED;
		auto index = StepInput(current.get(), infinity, chain.folds);
		if (!index.IsValid()) {
			break;
		}
		chain.infinity = MaxValue(chain.infinity, infinity);
		chain.path.push_back(index.GetIndex());
		current = *BucketRewriteInput(current.get(), index.GetIndex());
	}
	if (chain.path.empty()) {
		return false;
	}
	chain.input = current;
	return true;
}

class Evaluator {
public:
	Evaluator(ClientContext &context, const Chain &chain, const Domain &domain)
	    : domain(domain), expression(chain.root.get().Copy()), result(expression->GetReturnType()) {
		reference<unique_ptr<Expression>> slot(expression);
		for (auto index : chain.path) {
			slot = BucketRewriteInput(*slot.get(), index);
		}
		slot.get() = make_uniq<BoundReferenceExpression>(domain.type, 0);
		executor = make_uniq<ExpressionExecutor>(context, *expression);
		input.Initialize(Allocator::Get(context), vector<LogicalType> {domain.type});
	}

	const LogicalType &ResultType() const {
		return expression->GetReturnType();
	}

	bool Evaluate(const vector<int64_t> &keys, vector<Value> &out) {
		out.clear();
		try {
			idx_t offset = 0;
			while (offset < keys.size()) {
				const idx_t count = MinValue<idx_t>(keys.size() - offset, STANDARD_VECTOR_SIZE);
				input.Reset();
				input.SetCardinality(count);
				for (idx_t i = 0; i < count; i++) {
					input.SetValue(0, i, domain.ToValue(keys[offset + i]));
				}
				result.ResetFromCache(result_cache);
				executor->ExecuteExpression(input, result);
				for (idx_t i = 0; i < count; i++) {
					auto value = result.GetValue(i);
					if (value.IsNull()) {
						return false;
					}
					out.push_back(std::move(value));
				}
				offset += count;
			}
		} catch (...) {
			return false;
		}
		return true;
	}

	bool Evaluate(int64_t key, Value &out) {
		single_key.assign(1, key);
		if (!Evaluate(single_key, single_value)) {
			return false;
		}
		out = std::move(single_value[0]);
		return true;
	}

private:
	const Domain &domain;
	unique_ptr<Expression> expression;
	unique_ptr<ExpressionExecutor> executor;
	DataChunk input;
	Vector result;
	VectorCache result_cache {Allocator::DefaultAllocator(), result.GetType()};
	vector<int64_t> single_key;
	vector<Value> single_value;
};

struct Boundary {
	bool found = false;
	bool none = false;
	int64_t key = 0;
};

template <class PRED>
bool Probe(Evaluator &evaluator, PRED &pred, int64_t key, bool &result) {
	Value value;
	if (!evaluator.Evaluate(key, value)) {
		return false;
	}
	result = pred(value);
	return true;
}

enum class Bracket : uint8_t { FAILED, SETTLED, BRACKETED };

template <class PRED>
Bracket BracketBoundary(Evaluator &evaluator, const Domain &domain, PRED &pred, int64_t guess, int64_t &lo,
                        int64_t &hi, Boundary &boundary) {
	bool result = false;
	if (!Probe(evaluator, pred, guess, result)) {
		return Bracket::FAILED;
	}
	const bool upward = !result;
	const int64_t limit = upward ? domain.max_key : domain.min_key;
	lo = guess;
	hi = guess;
	int64_t key = guess;
	for (uint64_t step = 1;; step *= 2) {
		if (key == limit) {
			boundary.none = upward;
			boundary.found = !upward;
			boundary.key = domain.min_key;
			return Bracket::SETTLED;
		}
		const uint64_t room = upward ? uint64_t(limit) - uint64_t(key) : uint64_t(key) - uint64_t(limit);
		const int64_t next = room > step ? int64_t(upward ? uint64_t(key) + step : uint64_t(key) - step) : limit;
		if (!Probe(evaluator, pred, next, result)) {
			return Bracket::FAILED;
		}
		(result ? hi : lo) = next;
		if (result == upward) {
			return Bracket::BRACKETED;
		}
		key = next;
	}
}

template <class PRED>
bool RefineBoundary(Evaluator &evaluator, PRED &pred, int64_t lo, int64_t hi, Boundary &boundary) {
	vector<int64_t> keys;
	vector<Value> values;
	while (uint64_t(hi) - uint64_t(lo) > 1) {
		const uint64_t span = uint64_t(hi) - uint64_t(lo);
		const uint64_t points = MinValue<uint64_t>(span - 1, SEARCH_WIDTH);
		keys.clear();
		for (uint64_t i = 1; i <= points; i++) {
			keys.push_back(int64_t(uint64_t(lo) + (span / (points + 1)) * i));
		}
		if (!evaluator.Evaluate(keys, values)) {
			return false;
		}
		auto first_true = absl::c_find_if(values, [&](const Value &value) { return pred(value); });
		if (first_true == values.end()) {
			lo = keys.back();
		} else {
			const auto index = idx_t(first_true - values.begin());
			hi = keys[index];
			if (index > 0) {
				lo = keys[index - 1];
			}
		}
	}
	boundary.found = true;
	boundary.key = hi;
	return true;
}

template <class PRED>
bool FirstKey(Evaluator &evaluator, const Domain &domain, PRED pred, int64_t guess, Boundary &boundary) {
	int64_t lo = 0;
	int64_t hi = 0;
	switch (BracketBoundary(evaluator, domain, pred, guess, lo, hi, boundary)) {
	case Bracket::FAILED:
		return false;
	case Bracket::SETTLED:
		return true;
	default:
		return RefineBoundary(evaluator, pred, lo, hi, boundary);
	}
}

struct Bounds {
	Boundary lower;
	Boundary upper;
};

bool ZoneMonotone(ClientContext &context, Evaluator &evaluator, const Domain &domain, const Bounds &bounds) {
	if (domain.type.id() != LogicalTypeId::TIMESTAMP_TZ) {
		return true;
	}
	if (!MonotonePredicateRule::zone_transitions) {
		return false;
	}
	int64_t from = NumericLimits<int64_t>::Maximum();
	int64_t to = NumericLimits<int64_t>::Minimum();
	for (auto boundary : {bounds.lower, bounds.upper}) {
		if (!boundary.found) {
			continue;
		}
		from = MinValue(from, boundary.key);
		to = MaxValue(to, boundary.key);
	}
	if (from > to) {
		return true;
	}
	vector<int64_t> transitions;
	if (!MonotonePredicateRule::zone_transitions(context, from - TRANSITION_MARGIN, to + TRANSITION_MARGIN,
	                                              transitions)) {
		return false;
	}
	vector<int64_t> keys;
	for (auto transition : transitions) {
		keys.push_back(transition - 1);
		keys.push_back(transition);
	}
	vector<Value> values;
	if (!evaluator.Evaluate(keys, values)) {
		return false;
	}
	for (idx_t i = 0; i < values.size(); i += 2) {
		if (values[i] > values[i + 1]) {
			return false;
		}
	}
	return true;
}

unique_ptr<Expression> Compare(ExpressionType type, const Expression &input, Value value) {
	return BoundComparisonExpression::Create(type, input.Copy(), make_uniq<BoundConstantExpression>(std::move(value)));
}

unique_ptr<Expression> Conjoin(unique_ptr<Expression> left, unique_ptr<Expression> right) {
	if (!left) {
		return right;
	}
	if (!right) {
		return left;
	}
	return make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(left), std::move(right));
}

unique_ptr<Expression> Disjoin(unique_ptr<Expression> left, unique_ptr<Expression> right) {
	if (!left) {
		return right;
	}
	if (!right) {
		return left;
	}
	return make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(left), std::move(right));
}

struct RangeBuilder {
	const Domain &domain;
	const Expression &input;
	InfinityRule infinity;
	bool folds;

	unique_ptr<Expression> Key(ExpressionType type, int64_t key) const {
		return Compare(type, input, domain.ToValue(key));
	}
	unique_ptr<Expression> Infinite(ExpressionType type) const {
		return Compare(type, input, Value::Infinity(domain.type));
	}
	bool InhabitedInfinity() const {
		return infinity == InfinityRule::PRESERVED && domain.has_infinity;
	}
	bool ExcludedInfinity() const {
		return infinity == InfinityRule::NULLED && domain.has_infinity;
	}

	unique_ptr<Expression> Empty() const {
		return Conjoin(Key(ExpressionType::COMPARE_GREATERTHANOREQUALTO, domain.max_key),
		               Key(ExpressionType::COMPARE_LESSTHAN, domain.max_key));
	}
	unique_ptr<Expression> FiniteLower() const {
		return ExcludedInfinity() ? Key(ExpressionType::COMPARE_GREATERTHANOREQUALTO, domain.min_key) : nullptr;
	}
	unique_ptr<Expression> FiniteUpper() const {
		return ExcludedInfinity() ? Key(ExpressionType::COMPARE_LESSTHANOREQUALTO, domain.max_key) : nullptr;
	}
	unique_ptr<Expression> Top() const {
		return InhabitedInfinity() ? Infinite(ExpressionType::COMPARE_GREATERTHANOREQUALTO) : nullptr;
	}
	unique_ptr<Expression> Ceiling() const {
		return InhabitedInfinity() ? Infinite(ExpressionType::COMPARE_LESSTHAN)
		                           : Key(ExpressionType::COMPARE_LESSTHANOREQUALTO, domain.max_key);
	}
	unique_ptr<Expression> Nothing() const {
		if (infinity == InfinityRule::NULLED || domain.has_infinity) {
			return Empty();
		}
		return Key(ExpressionType::COMPARE_GREATERTHAN, domain.max_key);
	}

	unique_ptr<Expression> AtLeast(const Boundary &boundary) const {
		if (boundary.none) {
			auto top = Top();
			return top ? std::move(top) : Nothing();
		}
		return Conjoin(Key(ExpressionType::COMPARE_GREATERTHANOREQUALTO, boundary.key), FiniteUpper());
	}

	unique_ptr<Expression> Below(const Boundary &boundary) const {
		if (boundary.none) {
			auto finite = FiniteLower();
			if (!InhabitedInfinity() && !finite) {
				return nullptr;
			}
			return Conjoin(Ceiling(), std::move(finite));
		}
		return Conjoin(Key(ExpressionType::COMPARE_LESSTHAN, boundary.key), FiniteLower());
	}

	unique_ptr<Expression> Outside(const Boundary &lower, const Boundary &upper) const {
		if (lower.none) {
			return Below(lower);
		}
		if (upper.none) {
			return Disjoin(Below(lower), Top());
		}
		return Disjoin(Below(lower), AtLeast(upper));
	}

	unique_ptr<Expression> Between(const Boundary &lower, const Boundary &upper) const {
		if (lower.none) {
			return folds ? Empty() : nullptr;
		}
		auto result = Key(ExpressionType::COMPARE_GREATERTHANOREQUALTO, lower.key);
		if (upper.none) {
			return Conjoin(std::move(result), domain.has_infinity ? Ceiling() : nullptr);
		}
		return Conjoin(std::move(result), Key(ExpressionType::COMPARE_LESSTHAN, upper.key));
	}
};

struct Search {
	Search(ClientContext &context_p, Chain chain_p, Domain domain_p)
	    : context(context_p), chain(std::move(chain_p)), domain(std::move(domain_p)),
	      evaluator(context_p, chain, domain) {
	}

	const LogicalType &ResultType() const {
		return evaluator.ResultType();
	}
	RangeBuilder Builder() const {
		return RangeBuilder {domain, chain.input.get(), chain.infinity, chain.folds};
	}

	int64_t Guess(const Value &constant) const {
		try {
			Value guess = constant;
			if (!guess.DefaultTryCastAs(domain.type) || guess.IsNull()) {
				return 0;
			}
			if (domain.floating && !std::isfinite(guess.GetValueUnsafe<double>())) {
				return 0;
			}
			if (domain.type.IsTemporal() &&
			    (guess == Value::Infinity(domain.type) || guess == Value::NegativeInfinity(domain.type))) {
				return 0;
			}
			const auto key = domain.Key(guess);
			return key < domain.min_key || key > domain.max_key ? 0 : key;
		} catch (...) {
			return 0;
		}
	}

	bool Lower(const Value &constant, int64_t guess, Boundary &boundary) {
		return FirstKey(
		    evaluator, domain, [&](const Value &v) { return v >= constant; }, guess, boundary);
	}

	bool Upper(const Value &constant, int64_t guess, Boundary &boundary) {
		return FirstKey(
		    evaluator, domain, [&](const Value &v) { return v > constant; }, guess, boundary);
	}

	bool Verified(const Bounds &bounds) {
		return ZoneMonotone(context, evaluator, domain, bounds);
	}

	ClientContext &context;
	Chain chain;
	Domain domain;
	Evaluator evaluator;
};

class Rewriter {
public:
	Rewriter(ClientContext &context, bool filter_context) : context(context), filter_context(filter_context) {
	}

	unique_ptr<Expression> Rewrite(Expression &expr) {
		if (BoundComparisonExpression::IsComparison(expr)) {
			return Comparison(expr.Cast<BoundFunctionExpression>());
		}
		if (expr.GetExpressionType() == ExpressionType::COMPARE_BETWEEN &&
		    expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
			return Between(expr.Cast<BoundFunctionExpression>());
		}
		if (expr.GetExpressionType() == ExpressionType::COMPARE_IN &&
		    expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR) {
			return InList(expr.Cast<BoundOperatorExpression>());
		}
		return nullptr;
	}

private:
	unique_ptr<Search> Prepare(Expression &expr) {
		Chain chain(expr);
		if (!Analyse(expr, chain) || (chain.infinity == InfinityRule::NULLED && !filter_context)) {
			return nullptr;
		}
		auto &input = chain.input.get();
		if (input.IsVolatile() || !input.IsConsistent()) {
			return nullptr;
		}
		auto domain = Domain::For(chain.input.get().GetReturnType());
		if (!domain.valid) {
			return nullptr;
		}
		return make_uniq<Search>(context, std::move(chain), std::move(domain));
	}

	static bool IsInfinite(const Value &value) {
		const auto &type = value.type();
		if (type.IsTemporal()) {
			return value == Value::Infinity(type) || value == Value::NegativeInfinity(type);
		}
		if (type.id() == LogicalTypeId::DOUBLE) {
			const auto d = value.GetValueUnsafe<double>();
			return std::isinf(d);
		}
		return false;
	}

	bool Constant(const Expression &expr, const LogicalType &type, Value &value, bool &infinite) {
		infinite = false;
		if (!TryFoldConstant(context, expr, value)) {
			return false;
		}
		if (value.IsNull() || value.type() != type) {
			return false;
		}
		if (type.id() == LogicalTypeId::DOUBLE && std::isnan(value.GetValueUnsafe<double>())) {
			return false;
		}
		infinite = IsInfinite(value);
		return true;
	}

	bool Constant(const Expression &expr, const LogicalType &type, Value &value) {
		bool infinite = false;
		return Constant(expr, type, value, infinite) && !infinite;
	}

	unique_ptr<Expression> InfiniteConstant(const Search &search, ExpressionType type, const Value &constant) {
		const auto &domain = search.domain;
		if (search.chain.infinity != InfinityRule::PRESERVED || !domain.has_infinity || domain.floating) {
			return nullptr;
		}
		const bool positive = constant == Value::Infinity(search.ResultType());
		const auto &input = search.chain.input.get();
		const auto infinity = positive ? Value::Infinity(domain.type) : Value::NegativeInfinity(domain.type);
		switch (type) {
		case ExpressionType::COMPARE_EQUAL:
			return Compare(positive ? ExpressionType::COMPARE_GREATERTHANOREQUALTO
			                        : ExpressionType::COMPARE_LESSTHANOREQUALTO,
			               input, infinity);
		case ExpressionType::COMPARE_NOTEQUAL:
			return Compare(positive ? ExpressionType::COMPARE_LESSTHAN : ExpressionType::COMPARE_GREATERTHAN, input,
			               infinity);
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			return Compare(ExpressionType::COMPARE_GREATERTHANOREQUALTO, input, infinity);
		case ExpressionType::COMPARE_GREATERTHAN:
			return positive ? search.Builder().Empty() : Compare(ExpressionType::COMPARE_GREATERTHAN, input, infinity);
		case ExpressionType::COMPARE_LESSTHAN:
			return positive ? Compare(ExpressionType::COMPARE_LESSTHAN, input, infinity) : search.Builder().Empty();
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			return Compare(ExpressionType::COMPARE_LESSTHANOREQUALTO, input, infinity);
		default:
			return nullptr;
		}
	}

	unique_ptr<Expression> Comparison(BoundFunctionExpression &comparison) {
		auto type = comparison.GetExpressionType();
		auto &left = BoundComparisonExpression::LeftMutable(comparison);
		auto &right = BoundComparisonExpression::RightMutable(comparison);
		reference<Expression> function(*left);
		reference<Expression> constant_expr(*right);
		if (left->IsFoldable() && !right->IsFoldable()) {
			function = *right;
			constant_expr = *left;
			type = FlipComparisonExpression(type);
		} else if (!right->IsFoldable()) {
			return nullptr;
		}
		switch (type) {
		case ExpressionType::COMPARE_EQUAL:
		case ExpressionType::COMPARE_NOTEQUAL:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			break;
		default:
			return nullptr;
		}
		auto search = Prepare(function.get());
		if (!search) {
			return nullptr;
		}
		Value constant;
		bool infinite = false;
		if (!Constant(constant_expr.get(), search->ResultType(), constant, infinite)) {
			return nullptr;
		}
		if (infinite) {
			return InfiniteConstant(*search, type, constant);
		}
		Bounds bounds;
		const bool need_lower =
		    type != ExpressionType::COMPARE_GREATERTHAN && type != ExpressionType::COMPARE_LESSTHANOREQUALTO;
		const bool need_upper =
		    type != ExpressionType::COMPARE_GREATERTHANOREQUALTO && type != ExpressionType::COMPARE_LESSTHAN;
		const auto guess = search->Guess(constant);
		if ((need_lower && !search->Lower(constant, guess, bounds.lower)) ||
		    (need_upper && !search->Upper(constant, guess, bounds.upper)) || !search->Verified(bounds)) {
			return nullptr;
		}
		auto builder = search->Builder();
		switch (type) {
		case ExpressionType::COMPARE_EQUAL:
			return builder.Between(bounds.lower, bounds.upper);
		case ExpressionType::COMPARE_NOTEQUAL:
			return builder.Outside(bounds.lower, bounds.upper);
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			return builder.AtLeast(bounds.lower);
		case ExpressionType::COMPARE_GREATERTHAN:
			return builder.AtLeast(bounds.upper);
		case ExpressionType::COMPARE_LESSTHAN:
			return builder.Below(bounds.lower);
		default:
			return builder.Below(bounds.upper);
		}
	}

	unique_ptr<Expression> Between(BoundFunctionExpression &between) {
		auto &function = *BoundBetweenExpression::InputMutable(between);
		auto search = Prepare(function);
		if (!search) {
			return nullptr;
		}
		Value low, high;
		const auto &result_type = search->ResultType();
		if (!Constant(BoundBetweenExpression::LowerBound(between), result_type, low) ||
		    !Constant(BoundBetweenExpression::UpperBound(between), result_type, high)) {
			return nullptr;
		}
		Bounds bounds;
		const auto low_guess = search->Guess(low);
		const auto high_guess = search->Guess(high);
		const bool lower_ok = BoundBetweenExpression::LowerInclusive(between)
		                          ? search->Lower(low, low_guess, bounds.lower)
		                          : search->Upper(low, low_guess, bounds.lower);
		const bool upper_ok = BoundBetweenExpression::UpperInclusive(between)
		                          ? search->Upper(high, high_guess, bounds.upper)
		                          : search->Lower(high, high_guess, bounds.upper);
		if (!lower_ok || !upper_ok || !search->Verified(bounds)) {
			return nullptr;
		}
		return search->Builder().Between(bounds.lower, bounds.upper);
	}

	unique_ptr<Expression> InList(BoundOperatorExpression &in) {
		auto &children = in.GetChildrenMutable();
		if (children.size() < 2 || children.size() > 33) {
			return nullptr;
		}
		auto search = Prepare(*children[0]);
		if (!search) {
			return nullptr;
		}
		const auto &result_type = search->ResultType();
		vector<Value> constants;
		for (idx_t i = 1; i < children.size(); i++) {
			Value value;
			if (!Constant(*children[i], result_type, value)) {
				if (filter_context && TryFoldConstant(context, *children[i], value) && value.IsNull()) {
					continue;
				}
				return nullptr;
			}
			constants.push_back(std::move(value));
		}
		if (constants.empty()) {
			return nullptr;
		}
		unique_ptr<Expression> result;
		for (auto &constant : constants) {
			Bounds bounds;
			const auto guess = search->Guess(constant);
			if (!search->Lower(constant, guess, bounds.lower) || !search->Upper(constant, guess, bounds.upper) ||
			    !search->Verified(bounds)) {
				return nullptr;
			}
			auto range = search->Builder().Between(bounds.lower, bounds.upper);
			if (!range) {
				return nullptr;
			}
			result = result ? Disjoin(std::move(result), std::move(range)) : std::move(range);
		}
		return result;
	}

	ClientContext &context;
	bool filter_context;
};

void CollectLeafSlots(BoundConjunctionExpression &conjunction, vector<reference<unique_ptr<Expression>>> &slots) {
	for (auto &child : conjunction.GetChildrenMutable()) {
		if (child->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			CollectLeafSlots(child->Cast<BoundConjunctionExpression>(), slots);
		} else {
			slots.emplace_back(child);
		}
	}
}

unique_ptr<Expression> RewriteConjunction(Rewriter &rewriter, BoundConjunctionExpression &conjunction) {
	vector<reference<unique_ptr<Expression>>> leaves;
	CollectLeafSlots(conjunction, leaves);
	vector<unique_ptr<Expression>> replacements;
	bool changed = false;
	for (auto &leaf : leaves) {
		replacements.push_back(rewriter.Rewrite(*leaf.get()));
		changed = changed || replacements.back();
	}
	if (!changed) {
		return nullptr;
	}
	auto copy = conjunction.Copy();
	vector<reference<unique_ptr<Expression>>> slots;
	CollectLeafSlots(copy->Cast<BoundConjunctionExpression>(), slots);
	for (idx_t i = 0; i < slots.size(); i++) {
		if (replacements[i]) {
			slots[i].get() = std::move(replacements[i]);
		}
	}
	return copy;
}

} // namespace

MonotonePredicateRule::MonotonePredicateRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	root = make_uniq<ExpressionMatcher>();
}

unique_ptr<Expression> MonotonePredicateRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                    bool &changes_made, bool is_root) {
	auto &expr = bindings[0].get();
	const bool filter_context = is_root && op.type == LogicalOperatorType::LOGICAL_FILTER;
	Rewriter rewriter(GetContext(), filter_context);
	if (filter_context && expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		return RewriteConjunction(rewriter, expr.Cast<BoundConjunctionExpression>());
	}
	return rewriter.Rewrite(expr);
}

} // namespace duckdb
