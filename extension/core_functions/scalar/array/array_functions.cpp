#include "duckdb/common/vector/array_vector.hpp"
#include "core_functions/scalar/array_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

//------------------------------------------------------------------------------
// Element-wise combine functions
//------------------------------------------------------------------------------
// Given two arrays of the same size, combine their elements into a single array
// of the same size as the input arrays.
namespace {
struct CrossProductOp {
	template <class TYPE>
	static void Operation(const TYPE *lhs_data, const TYPE *rhs_data, TYPE *res_data, idx_t size) {
		D_ASSERT(size == 3);

		auto lx = lhs_data[0];
		auto ly = lhs_data[1];
		auto lz = lhs_data[2];

		auto rx = rhs_data[0];
		auto ry = rhs_data[1];
		auto rz = rhs_data[2];

		res_data[0] = ly * rz - lz * ry;
		res_data[1] = lz * rx - lx * rz;
		res_data[2] = lx * ry - ly * rx;
	}
};
} // namespace

template <class TYPE, class OP, idx_t N>
static void ArrayFixedCombine(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &lstate = state.Cast<ExecuteFunctionState>();
	const auto &expr = lstate.expr.Cast<BoundFunctionExpression>();
	const auto &func_name = expr.Function().GetName();

	const auto count = args.size();
	auto &lhs_child = ArrayVector::GetChildMutable(args.data[0]);
	auto &rhs_child = ArrayVector::GetChildMutable(args.data[1]);
	auto &res_child = ArrayVector::GetChildMutable(result);

	const auto &lhs_child_validity = FlatVector::Validity(lhs_child);
	const auto &rhs_child_validity = FlatVector::Validity(rhs_child);

	UnifiedVectorFormat lhs_format;
	UnifiedVectorFormat rhs_format;

	args.data[0].ToUnifiedFormat(lhs_format);
	args.data[1].ToUnifiedFormat(rhs_format);

	auto lhs_data = FlatVector::GetData<TYPE>(lhs_child);
	auto rhs_data = FlatVector::GetData<TYPE>(rhs_child);
	auto res_data = FlatVector::GetDataMutable<TYPE>(res_child);

	for (idx_t i = 0; i < count; i++) {
		const auto lhs_idx = lhs_format.sel->get_index(i);
		const auto rhs_idx = rhs_format.sel->get_index(i);

		if (!lhs_format.validity.RowIsValid(lhs_idx) || !rhs_format.validity.RowIsValid(rhs_idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		const auto left_offset = lhs_idx * N;
		if (!lhs_child_validity.CheckAllValid(left_offset + N, left_offset)) {
			throw InvalidInputException(
			    StringUtil::Format("%s: left argument can not contain NULL values", SQLIdentifier(func_name)));
		}

		const auto right_offset = rhs_idx * N;
		if (!rhs_child_validity.CheckAllValid(right_offset + N, right_offset)) {
			throw InvalidInputException(
			    StringUtil::Format("%s: right argument can not contain NULL values", SQLIdentifier(func_name)));
		}
		const auto result_offset = i * N;

		const auto lhs_data_ptr = lhs_data + left_offset;
		const auto rhs_data_ptr = rhs_data + right_offset;
		const auto res_data_ptr = res_data + result_offset;

		OP::Operation(lhs_data_ptr, rhs_data_ptr, res_data_ptr, N);
	}

	if (count == 1) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

ScalarFunctionSet ArrayCrossProductFun::GetFunctions() {
	ScalarFunctionSet set("array_cross_product");

	auto float_array = LogicalType::ARRAY(LogicalType::FLOAT, 3);
	auto double_array = LogicalType::ARRAY(LogicalType::DOUBLE, 3);

	ScalarFunction float_fun({}, float_array, ArrayFixedCombine<float, CrossProductOp, 3>);
	float_fun.GetSignature().AddParameter("array1", float_array).AddParameter("array2", float_array);
	set.AddFunction(float_fun);

	ScalarFunction double_fun({}, double_array, ArrayFixedCombine<double, CrossProductOp, 3>);
	double_fun.GetSignature().AddParameter("array1", double_array).AddParameter("array2", double_array);
	set.AddFunction(double_fun);

	set.SetFallible();

	return set;
}

} // namespace duckdb
