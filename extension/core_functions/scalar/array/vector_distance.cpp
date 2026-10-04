#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "core_functions/scalar/array_functions.hpp"
#include "core_functions/scalar/list_functions.hpp"
#include "core_functions/array_kernels.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/storage/statistics/array_stats.hpp"

namespace duckdb {

static const Identifier &BoundFunctionName(ExpressionState &state) {
	const auto &lstate = state.Cast<ExecuteFunctionState>();
	return lstate.expr.Cast<BoundFunctionExpression>().Function().GetName();
}

static void ThrowNullElements(const Identifier &func_name, const char *argument) {
	throw InvalidInputException(
	    StringUtil::Format("%s: %s can not contain NULL values", SQLIdentifier(func_name), argument));
}

template <class TYPE, class OP>
static void ListBinaryFold(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &func_name = BoundFunctionName(state);

	auto &lhs_vec = args.data[0];
	auto &rhs_vec = args.data[1];

	const auto lhs_count = ListVector::GetListSize(lhs_vec);
	const auto rhs_count = ListVector::GetListSize(rhs_vec);

	auto &lhs_child = ListVector::GetChildMutable(lhs_vec);
	auto &rhs_child = ListVector::GetChildMutable(rhs_vec);

	lhs_child.Flatten();
	rhs_child.Flatten();

	if (!FlatVector::ValidityMutable(lhs_child).CheckAllValid(lhs_count)) {
		ThrowNullElements(func_name, "left argument");
	}
	if (!FlatVector::ValidityMutable(rhs_child).CheckAllValid(rhs_count)) {
		ThrowNullElements(func_name, "right argument");
	}

	auto lhs_data = FlatVector::GetData<TYPE>(lhs_child);
	auto rhs_data = FlatVector::GetData<TYPE>(rhs_child);

	BinaryExecutor::Execute<list_entry_t, list_entry_t, TYPE>(
	    lhs_vec, rhs_vec, result, [&](const list_entry_t &left, const list_entry_t &right) -> optional<TYPE> {
		    if (left.length != right.length) {
			    throw InvalidInputException(
			        "%s: list dimensions must be equal, got left length '%d' and right length '%d'",
			        SQLIdentifier(func_name), left.length, right.length);
		    }
		    if (!OP::ALLOW_EMPTY && left.length == 0) {
			    return nullopt;
		    }
		    return OP::Operation(lhs_data + left.offset, rhs_data + right.offset, left.length);
	    });
}

static bool TakesOtherArgumentType(const Expression &argument) {
	const auto type = ExpressionBinder::GetExpressionReturnType(argument);
	return type.IsUnknown() || type.id() == LogicalTypeId::STRING_LITERAL;
}

static unique_ptr<FunctionData> ArrayBinaryBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	const auto &lhs_type = arguments[0]->GetReturnType();
	const auto &rhs_type = arguments[1]->GetReturnType();

	if (lhs_type.IsUnknown() && rhs_type.IsUnknown()) {
		bound_function.GetArguments()[0] = rhs_type;
		bound_function.GetArguments()[1] = lhs_type;
		bound_function.SetReturnType(LogicalType::UNKNOWN);
		return nullptr;
	}

	bound_function.GetArguments()[0] = TakesOtherArgumentType(*arguments[0]) ? rhs_type : lhs_type;
	bound_function.GetArguments()[1] = TakesOtherArgumentType(*arguments[1]) ? lhs_type : rhs_type;

	if (bound_function.GetArguments()[0].id() != LogicalTypeId::ARRAY ||
	    bound_function.GetArguments()[1].id() != LogicalTypeId::ARRAY) {
		throw InvalidInputException(StringUtil::Format("%s: Arguments must be arrays of FLOAT or DOUBLE",
		                                               SQLIdentifier(bound_function.GetName())));
	}

	const auto lhs_size = ArrayType::GetSize(bound_function.GetArguments()[0]);
	const auto rhs_size = ArrayType::GetSize(bound_function.GetArguments()[1]);

	if (lhs_size != rhs_size) {
		throw BinderException("%s: Array arguments must be of the same size", SQLIdentifier(bound_function.GetName()));
	}

	const auto &lhs_element_type = ArrayType::GetChildType(bound_function.GetArguments()[0]);
	const auto &rhs_element_type = ArrayType::GetChildType(bound_function.GetArguments()[1]);

	LogicalType common_type;
	if (!LogicalType::TryGetMaxLogicalType(context, lhs_element_type, rhs_element_type, common_type)) {
		throw BinderException("%s: Cannot infer common element type (left = '%s', right = '%s')",
		                      SQLIdentifier(bound_function.GetName()), lhs_element_type.ToString(),
		                      rhs_element_type.ToString());
	}

	if (common_type.id() != LogicalTypeId::FLOAT && common_type.id() != LogicalTypeId::DOUBLE) {
		throw BinderException("%s: Arguments must be arrays of FLOAT or DOUBLE",
		                      SQLIdentifier(bound_function.GetName()));
	}

	bound_function.GetArguments()[0] = LogicalType::ARRAY(common_type, lhs_size);
	bound_function.GetArguments()[1] = LogicalType::ARRAY(common_type, rhs_size);

	return nullptr;
}

template <class TYPE, class OP>
static void ArrayBinaryFold(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &func_name = BoundFunctionName(state);

	const auto count = args.size();
	auto &lhs_child = ArrayVector::GetChildMutable(args.data[0]);
	auto &rhs_child = ArrayVector::GetChildMutable(args.data[1]);

	const auto &lhs_child_validity = FlatVector::Validity(lhs_child);
	const auto &rhs_child_validity = FlatVector::Validity(rhs_child);

	UnifiedVectorFormat lhs_format;
	UnifiedVectorFormat rhs_format;

	args.data[0].ToUnifiedFormat(lhs_format);
	args.data[1].ToUnifiedFormat(rhs_format);

	auto lhs_data = FlatVector::GetData<TYPE>(lhs_child);
	auto rhs_data = FlatVector::GetData<TYPE>(rhs_child);
	auto res_data = FlatVector::GetDataMutable<TYPE>(result);

	const auto array_size = ArrayType::GetSize(args.data[0].GetType());
	D_ASSERT(array_size == ArrayType::GetSize(args.data[1].GetType()));

	for (idx_t i = 0; i < count; i++) {
		const auto lhs_idx = lhs_format.sel->get_index(i);
		const auto rhs_idx = rhs_format.sel->get_index(i);

		if (!lhs_format.validity.RowIsValid(lhs_idx) || !rhs_format.validity.RowIsValid(rhs_idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		const auto left_offset = lhs_idx * array_size;
		if (!lhs_child_validity.CheckAllValid(left_offset + array_size, left_offset)) {
			ThrowNullElements(func_name, "left argument");
		}

		const auto right_offset = rhs_idx * array_size;
		if (!rhs_child_validity.CheckAllValid(right_offset + array_size, right_offset)) {
			ThrowNullElements(func_name, "right argument");
		}

		res_data[i] = OP::Operation(lhs_data + left_offset, rhs_data + right_offset, array_size);
	}

	if (count == 1) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

static auto ArrayBinaryFoldStats(ClientContext &context, FunctionStatisticsInput &input) -> unique_ptr<BaseStatistics> {
	const auto &lhs_stats = input.child_stats[0];
	const auto &rhs_stats = input.child_stats[1];
	auto new_stats = NumericStats::CreateUnknown(input.expr.GetReturnType());
	new_stats.CombineValidity(lhs_stats, rhs_stats);
	if (!lhs_stats.CanHaveNoNull() || !rhs_stats.CanHaveNoNull()) {
		new_stats.Set(StatsInfo::CANNOT_HAVE_VALID_VALUES);
	}

	auto &lhs_child_stats = ArrayStats::GetChildStats(lhs_stats);
	auto &rhs_child_stats = ArrayStats::GetChildStats(rhs_stats);

	if (lhs_child_stats.CanHaveNull() || rhs_child_stats.CanHaveNull()) {
		return new_stats.ToUnique();
	}

	input.expr.FunctionMutable().GetProperties().SetErrorMode(FunctionErrors::CANNOT_ERROR);

	return new_stats.ToUnique();
}

template <class OP>
static ScalarFunctionSet BinaryFoldSet() {
	ScalarFunctionSet set;
	for (auto &type : LogicalType::Real()) {
		const auto is_float = type.id() == LogicalTypeId::FLOAT;

		const auto list = LogicalType::LIST(type);
		ScalarFunction list_fun({}, type, is_float ? ListBinaryFold<float, OP> : ListBinaryFold<double, OP>);
		list_fun.SetFallible();
		list_fun.GetSignature().AddParameter("list1", list).AddParameter("list2", list);
		set.AddFunction(list_fun);

		const auto array = LogicalType::ARRAY(type, optional_idx());
		ScalarFunction array_fun({}, type, is_float ? ArrayBinaryFold<float, OP> : ArrayBinaryFold<double, OP>,
		                         ArrayBinaryBind, ArrayBinaryFoldStats);
		array_fun.SetFallible();
		array_fun.GetSignature().AddParameter("array1", array).AddParameter("array2", array);
		set.AddFunction(array_fun);
	}
	return set;
}

template <class TYPE, class OP>
static void ListNorm(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &input = args.data[0];
	auto &child = ListVector::GetChildMutable(input);
	child.Flatten();
	if (!FlatVector::ValidityMutable(child).CheckAllValid(ListVector::GetListSize(input))) {
		ThrowNullElements(BoundFunctionName(state), "argument");
	}
	auto data = FlatVector::GetData<TYPE>(child);
	UnaryExecutor::Execute<list_entry_t, TYPE>(input, result, args.size(), [&](const list_entry_t &entry) {
		return OP::Operation(data + entry.offset, entry.length);
	});
}

template <class TYPE, class OP>
static void ArrayNorm(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &input = args.data[0];
	const auto count = args.size();
	auto &child = ArrayVector::GetChildMutable(input);
	const auto &child_validity = FlatVector::Validity(child);
	const auto array_size = ArrayType::GetSize(input.GetType());

	UnifiedVectorFormat format;
	input.ToUnifiedFormat(format);

	auto data = FlatVector::GetData<TYPE>(child);
	auto res_data = FlatVector::GetDataMutable<TYPE>(result);

	for (idx_t i = 0; i < count; i++) {
		const auto idx = format.sel->get_index(i);
		if (!format.validity.RowIsValid(idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		const auto offset = idx * array_size;
		if (!child_validity.CheckAllValid(offset + array_size, offset)) {
			ThrowNullElements(BoundFunctionName(state), "argument");
		}
		res_data[i] = OP::Operation(data + offset, array_size);
	}

	if (count == 1) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

template <class TYPE, class OP>
static void ListNormalize(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &input = args.data[0];
	const auto count = args.size();
	auto &child = ListVector::GetChildMutable(input);
	child.Flatten();
	if (!FlatVector::ValidityMutable(child).CheckAllValid(ListVector::GetListSize(input))) {
		ThrowNullElements(BoundFunctionName(state), "argument");
	}

	UnifiedVectorFormat format;
	input.ToUnifiedFormat(format);
	auto entries = UnifiedVectorFormat::GetData<list_entry_t>(format);
	auto data = FlatVector::GetData<TYPE>(child);

	idx_t total = 0;
	for (idx_t i = 0; i < count; i++) {
		const auto idx = format.sel->get_index(i);
		if (format.validity.RowIsValid(idx)) {
			total += entries[idx].length;
		}
	}
	ListVector::Reserve(result, total);
	auto res_entries = FlatVector::GetDataMutable<list_entry_t>(result);
	auto res_data = FlatVector::GetDataMutable<TYPE>(ListVector::GetChildMutable(result));

	idx_t offset = 0;
	for (idx_t i = 0; i < count; i++) {
		const auto idx = format.sel->get_index(i);
		if (!format.validity.RowIsValid(idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		const auto &entry = entries[idx];
		res_entries[i] = list_entry_t(offset, entry.length);
		OP::Operation(data + entry.offset, res_data + offset, entry.length);
		offset += entry.length;
	}
	ListVector::SetListSize(result, offset);

	if (count == 1) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

template <class TYPE, class OP>
static void ArrayNormalize(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &input = args.data[0];
	const auto count = args.size();
	auto &child = ArrayVector::GetChildMutable(input);
	const auto &child_validity = FlatVector::Validity(child);
	const auto array_size = ArrayType::GetSize(input.GetType());

	UnifiedVectorFormat format;
	input.ToUnifiedFormat(format);

	auto data = FlatVector::GetData<TYPE>(child);
	auto &res_child = ArrayVector::GetChildMutable(result);
	auto res_data = FlatVector::GetDataMutable<TYPE>(res_child);

	for (idx_t i = 0; i < count; i++) {
		const auto idx = format.sel->get_index(i);
		if (!format.validity.RowIsValid(idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		const auto offset = idx * array_size;
		if (!child_validity.CheckAllValid(offset + array_size, offset)) {
			ThrowNullElements(BoundFunctionName(state), "argument");
		}
		OP::Operation(data + offset, res_data + i * array_size, array_size);
	}

	if (count == 1) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

static unique_ptr<FunctionData> ArrayNormalizeBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	bound_function.SetReturnType(input.GetArguments()[0]->GetReturnType());
	return nullptr;
}

template <class OP>
static ScalarFunctionSet NormSet() {
	ScalarFunctionSet set;
	for (auto &type : LogicalType::Real()) {
		const auto is_float = type.id() == LogicalTypeId::FLOAT;

		ScalarFunction list_fun({}, type, is_float ? ListNorm<float, OP> : ListNorm<double, OP>);
		list_fun.SetFallible();
		list_fun.GetSignature().AddParameter("list", LogicalType::LIST(type));
		set.AddFunction(list_fun);

		ScalarFunction array_fun({}, type, is_float ? ArrayNorm<float, OP> : ArrayNorm<double, OP>);
		array_fun.SetFallible();
		array_fun.GetSignature().AddParameter("array", LogicalType::ARRAY(type, optional_idx()));
		set.AddFunction(array_fun);
	}
	return set;
}

template <class OP>
static ScalarFunctionSet NormalizeSet() {
	ScalarFunctionSet set;
	for (auto &type : LogicalType::Real()) {
		const auto is_float = type.id() == LogicalTypeId::FLOAT;

		const auto list = LogicalType::LIST(type);
		ScalarFunction list_fun({}, list, is_float ? ListNormalize<float, OP> : ListNormalize<double, OP>);
		list_fun.SetFallible();
		list_fun.GetSignature().AddParameter("list", list);
		set.AddFunction(list_fun);

		const auto array = LogicalType::ARRAY(type, optional_idx());
		ScalarFunction array_fun({}, array, is_float ? ArrayNormalize<float, OP> : ArrayNormalize<double, OP>,
		                         ArrayNormalizeBind);
		array_fun.SetFallible();
		array_fun.GetSignature().AddParameter("array", array);
		set.AddFunction(array_fun);
	}
	return set;
}

ScalarFunctionSet L2DistanceFun::GetFunctions() {
	return BinaryFoldSet<DistanceOp>();
}

ScalarFunctionSet L2SqrDistanceFun::GetFunctions() {
	return BinaryFoldSet<DistanceSquaredOp>();
}

ScalarFunctionSet L1DistanceFun::GetFunctions() {
	return BinaryFoldSet<L1DistanceOp>();
}

ScalarFunctionSet InnerProductFun::GetFunctions() {
	return BinaryFoldSet<InnerProductOp>();
}

ScalarFunctionSet NegativeInnerProductFun::GetFunctions() {
	return BinaryFoldSet<NegativeInnerProductOp>();
}

ScalarFunctionSet CosineSimilarityFun::GetFunctions() {
	return BinaryFoldSet<CosineSimilarityOp>();
}

ScalarFunctionSet CosineDistanceFun::GetFunctions() {
	return BinaryFoldSet<CosineDistanceOp>();
}

ScalarFunctionSet L1NormFun::GetFunctions() {
	return NormSet<L1NormOp>();
}

ScalarFunctionSet L2NormFun::GetFunctions() {
	return NormSet<L2NormOp>();
}

ScalarFunctionSet L1NormalizeFun::GetFunctions() {
	return NormalizeSet<L1NormalizeOp>();
}

ScalarFunctionSet L2NormalizeFun::GetFunctions() {
	return NormalizeSet<L2NormalizeOp>();
}

ScalarFunctionSet ListDistanceFun::GetFunctions() {
	return L2DistanceFun::GetFunctions();
}

ScalarFunctionSet ArrayDistanceFun::GetFunctions() {
	return L2DistanceFun::GetFunctions();
}

ScalarFunctionSet ListInnerProductFun::GetFunctions() {
	return InnerProductFun::GetFunctions();
}

ScalarFunctionSet ArrayInnerProductFun::GetFunctions() {
	return InnerProductFun::GetFunctions();
}

ScalarFunctionSet ListNegativeInnerProductFun::GetFunctions() {
	return NegativeInnerProductFun::GetFunctions();
}

ScalarFunctionSet ArrayNegativeInnerProductFun::GetFunctions() {
	return NegativeInnerProductFun::GetFunctions();
}

ScalarFunctionSet ListCosineSimilarityFun::GetFunctions() {
	return CosineSimilarityFun::GetFunctions();
}

ScalarFunctionSet ArrayCosineSimilarityFun::GetFunctions() {
	return CosineSimilarityFun::GetFunctions();
}

ScalarFunctionSet ListCosineDistanceFun::GetFunctions() {
	return CosineDistanceFun::GetFunctions();
}

ScalarFunctionSet ArrayCosineDistanceFun::GetFunctions() {
	return CosineDistanceFun::GetFunctions();
}

} // namespace duckdb
