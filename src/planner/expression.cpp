#include "duckdb/planner/expression.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/parser/expression_util.hpp"

namespace duckdb {

Expression::Expression(ExpressionType type, ExpressionClass expression_class, LogicalType return_type)
    : BaseExpression(type, expression_class), return_type(std::move(return_type)) {
}

unique_ptr<Expression> Expression::PreserveReturnType(const LogicalType &type, unique_ptr<Expression> replacement) {
	D_ASSERT(replacement);
	D_ASSERT(replacement->GetReturnType() == type);
	replacement->SetReturnType(type);
	return replacement;
}

Expression::~Expression() {
}

bool Expression::IsAggregate() const {
	return ExpressionIterator::AnyChild(*this, [](const Expression &child) { return child.IsAggregate(); });
}

bool Expression::IsWindow() const {
	return ExpressionIterator::AnyChild(*this, [](const Expression &child) { return child.IsWindow(); });
}

bool Expression::IsScalar() const {
	return ExpressionIterator::AllChild(*this, [](const Expression &child) { return child.IsScalar(); });
}

bool Expression::IsVolatile() const {
	return IsVolatile([](const Expression &child) { return child.IsVolatile(); });
}

bool Expression::IsConsistent() const {
	return ExpressionIterator::AllChild(*this, [](const Expression &child) { return child.IsConsistent(); });
}

bool Expression::CanThrow() const {
	return ExpressionIterator::AnyChild(*this, [](const Expression &child) { return child.CanThrow(); });
}

bool Expression::RequiresOrderedExecution() const {
	return ExpressionIterator::AnyChild(*this,
	                                    [](const Expression &child) { return child.RequiresOrderedExecution(); });
}

bool Expression::PropagatesNullValues() const {
	if (type == ExpressionType::OPERATOR_IS_NULL || type == ExpressionType::OPERATOR_IS_NOT_NULL ||
	    type == ExpressionType::COMPARE_NOT_DISTINCT_FROM || type == ExpressionType::COMPARE_DISTINCT_FROM ||
	    type == ExpressionType::CONJUNCTION_OR || type == ExpressionType::CONJUNCTION_AND ||
	    type == ExpressionType::OPERATOR_COALESCE || type == ExpressionType::CASE_EXPR) {
		return false;
	}
	return ExpressionIterator::AllChild(*this, [](const Expression &child) { return child.PropagatesNullValues(); });
}

bool Expression::IsNeverFoldable(ExpressionClass expression_class) {
	switch (expression_class) {
	case ExpressionClass::BOUND_AGGREGATE:
	case ExpressionClass::BOUND_COLUMN_REF:
	case ExpressionClass::BOUND_DEFAULT:
	case ExpressionClass::BOUND_LAMBDA_REF:
	case ExpressionClass::BOUND_PARAMETER:
	case ExpressionClass::BOUND_REF:
	case ExpressionClass::BOUND_SUBQUERY:
	case ExpressionClass::BOUND_UNNEST:
	case ExpressionClass::BOUND_WINDOW:
		return true;
	default:
		return false;
	}
}

bool Expression::IsFoldable() const {
	if (ExpressionIterator::AnyChild(*this, [](const Expression &child) {
		    if (!IsNeverFoldable(child.GetExpressionClass())) {
			    return false;
		    }
		    D_ASSERT(!child.IsFoldable());
		    return true;
	    })) {
		return false;
	}
	return ExpressionIterator::AllChild(*this, [](const Expression &child) { return child.IsFoldable(); });
}

bool Expression::HasParameter() const {
	return ExpressionIterator::AnyChild(*this, [](const Expression &child) { return child.HasParameter(); });
}

bool Expression::HasSubquery() const {
	return ExpressionIterator::AnyChild(*this, [](const Expression &child) { return child.HasSubquery(); });
}

hash_t Expression::Hash() const {
	return Hash([](const Expression &child) { return child.Hash(); });
}

hash_t Expression::NodeHash() const {
	return CombineHash(duckdb::Hash<uint32_t>(static_cast<uint32_t>(type)), return_type.Hash());
}

bool Expression::Equals(const unique_ptr<Expression> &left, const unique_ptr<Expression> &right) {
	if (left.get() == right.get()) {
		return true;
	}
	if (!left || !right) {
		return false;
	}
	return left->Equals(*right);
}

bool Expression::ListEquals(const vector<unique_ptr<Expression>> &left, const vector<unique_ptr<Expression>> &right) {
	return ExpressionUtil::ListEquals(left, right);
}

void Expression::SetVerificationStats(unique_ptr<BaseStatistics> stats) {
	verification_stats = std::move(stats);
}

} // namespace duckdb
