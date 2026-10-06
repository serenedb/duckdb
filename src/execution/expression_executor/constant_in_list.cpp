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

#include "duckdb/execution/constant_in_list.hpp"

#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

namespace duckdb {

namespace {

template <class T>
bool Execute(const BoundOperatorExpression &expr, Vector &left, idx_t count, Vector &result, bool negate) {
	vector<T> constants;
	vector<string> owned;
	bool has_null = false;
	auto &children = expr.GetChildren();
	if (std::is_same<T, string_t>::value) {
		owned.reserve(children.size());
	}
	for (idx_t i = 1; i < children.size(); i++) {
		auto &value = children[i]->Cast<BoundConstantExpression>().GetValue();
		if (value.IsNull()) {
			has_null = true;
			continue;
		}
		if constexpr (std::is_same<T, string_t>::value) {
			owned.push_back(StringValue::Get(value));
			constants.push_back(string_t(owned.back()));
		} else {
			constants.push_back(value.GetValueUnsafe<T>());
		}
	}
	UnifiedVectorFormat format;
	left.ToUnifiedFormat(format);
	auto data = UnifiedVectorFormat::GetData<T>(format);
	auto writer = FlatVector::Writer<bool>(result, count);
	for (idx_t row = 0; row < count; row++) {
		auto idx = format.sel->get_index(row);
		if (!format.validity.RowIsValid(idx)) {
			writer.WriteNull();
			continue;
		}
		bool found = false;
		for (const auto &constant : constants) {
			found |= Equals::Operation<T>(data[idx], constant);
		}
		if (!found && has_null) {
			writer.WriteNull();
			continue;
		}
		writer.WriteValue(found != negate);
	}
	return true;
}

} // namespace

bool ConstantInList::Supports(const LogicalType &type) {
	switch (type.InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::INT128:
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
	case PhysicalType::UINT128:
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
		return true;
	case PhysicalType::VARCHAR:
		return StringType::GetCollation(type).empty();
	default:
		return false;
	}
}

bool ConstantInList::TryExecute(const BoundOperatorExpression &expr, Vector &left, idx_t count, Vector &result) {
	auto &children = expr.GetChildren();
	auto &type = left.GetType();
	if (children.size() < 2 || children.size() - 1 > MAX_CONSTANTS || !Supports(type)) {
		return false;
	}
	for (idx_t i = 1; i < children.size(); i++) {
		if (children[i]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
		    children[i]->GetReturnType() != type) {
			return false;
		}
	}
	const bool negate = expr.GetExpressionType() == ExpressionType::COMPARE_NOT_IN;
	switch (type.InternalType()) {
	case PhysicalType::BOOL:
		return Execute<bool>(expr, left, count, result, negate);
	case PhysicalType::INT8:
		return Execute<int8_t>(expr, left, count, result, negate);
	case PhysicalType::INT16:
		return Execute<int16_t>(expr, left, count, result, negate);
	case PhysicalType::INT32:
		return Execute<int32_t>(expr, left, count, result, negate);
	case PhysicalType::INT64:
		return Execute<int64_t>(expr, left, count, result, negate);
	case PhysicalType::INT128:
		return Execute<hugeint_t>(expr, left, count, result, negate);
	case PhysicalType::UINT8:
		return Execute<uint8_t>(expr, left, count, result, negate);
	case PhysicalType::UINT16:
		return Execute<uint16_t>(expr, left, count, result, negate);
	case PhysicalType::UINT32:
		return Execute<uint32_t>(expr, left, count, result, negate);
	case PhysicalType::UINT64:
		return Execute<uint64_t>(expr, left, count, result, negate);
	case PhysicalType::UINT128:
		return Execute<uhugeint_t>(expr, left, count, result, negate);
	case PhysicalType::FLOAT:
		return Execute<float>(expr, left, count, result, negate);
	case PhysicalType::DOUBLE:
		return Execute<double>(expr, left, count, result, negate);
	case PhysicalType::VARCHAR:
		return Execute<string_t>(expr, left, count, result, negate);
	default:
		return false;
	}
}

} // namespace duckdb
