//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/bound_constant_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/value.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class BoundConstantExpression : public Expression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::BOUND_CONSTANT;

public:
	explicit BoundConstantExpression(Value value);

public:
	const Value &GetValue() const {
		return value;
	}
	void SetValue(Value value_p);
	Value TakeValue();
	void SetReturnType(LogicalType type) override;
	string ToString() const override;

	bool Equals(const BaseExpression &other) const override;
	using Expression::Hash;
	unique_ptr<Expression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<Expression> Deserialize(Deserializer &deserializer);

protected:
	hash_t NodeHash() const override;

private:
	Value value;
};
} // namespace duckdb
