#include "duckdb/formats/raw_predicates.hpp"

#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"

#include <algorithm>

namespace duckdb {

namespace {

bool IsStringColumnRef(const Expression &expr) {
	return ExpressionFilter::IsSimpleFilterColumnRef(expr) && expr.GetReturnType().id() == LogicalTypeId::VARCHAR &&
	       !expr.GetReturnType().HasAlias();
}

const string *StringConstant(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return nullptr;
	}
	auto &value = expr.Cast<BoundConstantExpression>().GetValue();
	if (value.IsNull() || value.type().id() != LogicalTypeId::VARCHAR) {
		return nullptr;
	}
	return &StringValue::Get(value);
}

bool RawLike(const RawPredicate &predicate, std::string_view field) {
	auto &segments = predicate.values;
	idx_t first = 0;
	idx_t last = segments.size();
	idx_t begin = 0;
	idx_t end = field.size();
	if (predicate.anchored_start && last > 0) {
		if (!field.starts_with(segments[0])) {
			return false;
		}
		begin = segments[0].size();
		first = 1;
	}
	if (predicate.anchored_end) {
		if (first == last) {
			return begin == end;
		}
		auto &segment = segments[last - 1];
		if (end - begin < segment.size() || !field.ends_with(segment)) {
			return false;
		}
		end -= segment.size();
		last--;
	}
	const auto window = field.substr(0, end);
	for (idx_t i = first; i < last; i++) {
		const auto position = window.find(segments[i], begin);
		if (position == std::string_view::npos) {
			return false;
		}
		begin = position + segments[i].size();
	}
	return true;
}

bool RawCompare(ExpressionType type, std::string_view field, const string &constant) {
	const string_t left(field.data(), UnsafeNumericCast<uint32_t>(field.size()));
	const string_t right(constant.data(), UnsafeNumericCast<uint32_t>(constant.size()));
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		return Equals::Operation(left, right);
	case ExpressionType::COMPARE_NOTEQUAL:
		return NotEquals::Operation(left, right);
	case ExpressionType::COMPARE_LESSTHAN:
		return LessThan::Operation(left, right);
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return LessThanEquals::Operation(left, right);
	case ExpressionType::COMPARE_GREATERTHAN:
		return GreaterThan::Operation(left, right);
	default:
		return GreaterThanEquals::Operation(left, right);
	}
}

} // namespace

bool CollectRawPattern(const Expression &expr, bool negated, vector<RawPredicate> &out) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &function = expr.Cast<BoundFunctionExpression>();
	auto &children = function.GetChildren();
	if (children.size() != 2 || !IsStringColumnRef(*children[0])) {
		return false;
	}
	auto pattern = StringConstant(*children[1]);
	if (!pattern) {
		return false;
	}
	auto &name = function.Function().GetName();
	RawPredicate predicate {negated ? RawPredicateKind::NOT_LIKE : RawPredicateKind::LIKE};
	if (name == "prefix" || name == "starts_with" || name == "^@") {
		predicate.anchored_start = true;
		predicate.values.push_back(*pattern);
	} else if (name == "suffix" || name == "ends_with") {
		predicate.anchored_end = true;
		predicate.values.push_back(*pattern);
	} else if (name == "contains") {
		predicate.values.push_back(*pattern);
	} else if (name == "~~" || name == "!~~") {
		if (pattern->find_first_of("_\\") != string::npos) {
			return false;
		}
		if (name == "!~~") {
			predicate.kind = negated ? RawPredicateKind::LIKE : RawPredicateKind::NOT_LIKE;
		}
		predicate.anchored_start = pattern->empty() || pattern->front() != '%';
		predicate.anchored_end = pattern->empty() || pattern->back() != '%';
		idx_t begin = 0;
		while (begin <= pattern->size()) {
			auto end = std::min(pattern->find('%', begin), pattern->size());
			if (end > begin) {
				predicate.values.push_back(pattern->substr(begin, end - begin));
			}
			begin = end + 1;
		}
	} else {
		return false;
	}
	out.push_back(std::move(predicate));
	return true;
}

bool CollectRawPredicates(const Expression &expr, bool string_column, vector<RawPredicate> &out) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		if (expr.GetExpressionType() != ExpressionType::CONJUNCTION_AND) {
			return false;
		}
		for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
			if (!CollectRawPredicates(*child, string_column, out)) {
				return false;
			}
		}
		return true;
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op = expr.Cast<BoundOperatorExpression>();
		const auto type = op.GetExpressionType();
		auto &children = op.GetChildren();
		if (type == ExpressionType::OPERATOR_NOT) {
			return string_column && children.size() == 1 && CollectRawPattern(*children[0], true, out);
		}
		if (children.empty() || !ExpressionFilter::IsSimpleFilterColumnRef(*children[0])) {
			return false;
		}
		if (type == ExpressionType::OPERATOR_IS_NULL || type == ExpressionType::OPERATOR_IS_NOT_NULL) {
			out.push_back(
			    {type == ExpressionType::OPERATOR_IS_NULL ? RawPredicateKind::IS_NULL : RawPredicateKind::IS_NOT_NULL});
			return true;
		}
		if (type != ExpressionType::COMPARE_IN || !string_column) {
			return false;
		}
		RawPredicate predicate {RawPredicateKind::IN_LIST};
		for (idx_t i = 1; i < children.size(); i++) {
			auto value = StringConstant(*children[i]);
			if (!value) {
				return false;
			}
			predicate.values.push_back(*value);
		}
		std::sort(predicate.values.begin(), predicate.values.end());
		out.push_back(std::move(predicate));
		return true;
	}
	case ExpressionClass::BOUND_FUNCTION: {
		if (!string_column) {
			return false;
		}
		auto &function = expr.Cast<BoundFunctionExpression>();
		auto type = function.GetExpressionType();
		if (type == ExpressionType::BOUND_FUNCTION) {
			return CollectRawPattern(function, false, out);
		}
		if (!BoundComparisonExpression::IsComparison(type) || type == ExpressionType::COMPARE_DISTINCT_FROM ||
		    type == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			return false;
		}
		auto constant = ExpressionFilter::TryGetColumnConstantComparison(function, type);
		if (!constant) {
			return false;
		}
		auto &value = constant->GetValue();
		if (value.IsNull() || value.type().id() != LogicalTypeId::VARCHAR) {
			return false;
		}
		out.push_back({RawPredicateKind::COMPARE, type, {StringValue::Get(value)}});
		return true;
	}
	default:
		return false;
	}
}

bool RawPasses(const vector<RawPredicate> &predicates, const char *value, idx_t size, bool null) {
	for (auto &predicate : predicates) {
		if (predicate.kind == RawPredicateKind::IS_NULL) {
			if (!null) {
				return false;
			}
			continue;
		}
		if (null) {
			return false;
		}
		const std::string_view field(value, size);
		switch (predicate.kind) {
		case RawPredicateKind::IS_NOT_NULL:
			break;
		case RawPredicateKind::IN_LIST:
			if (!std::binary_search(predicate.values.begin(), predicate.values.end(), field)) {
				return false;
			}
			break;
		case RawPredicateKind::LIKE:
		case RawPredicateKind::NOT_LIKE:
			if (RawLike(predicate, field) != (predicate.kind == RawPredicateKind::LIKE)) {
				return false;
			}
			break;
		default:
			if (!RawCompare(predicate.compare, field, predicate.values[0])) {
				return false;
			}
			break;
		}
	}
	return true;
}

} // namespace duckdb
