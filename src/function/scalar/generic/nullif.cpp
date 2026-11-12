#include "duckdb/function/scalar/generic_functions.hpp"

#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

namespace {

struct NullIfBindData final : public FunctionData {
	explicit NullIfBindData(unique_ptr<Expression> collation_p) : collation(std::move(collation_p)) {
	}

	unique_ptr<Expression> collation;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<NullIfBindData>(collation->Copy());
	}

	bool Equals(const FunctionData &other) const override {
		return collation->Equals(*other.Cast<NullIfBindData>().collation);
	}
};

struct NullIfLocalState final : public FunctionLocalState {
	NullIfLocalState(ExpressionState &state, const Expression &collation, const LogicalType &type)
	    : executor(state, collation) {
		input.InitializeEmpty({type});
		auto &collated_type = collation.GetReturnType();
		collated.Initialize(executor.GetAllocator(), {collated_type, collated_type});
	}

	static unique_ptr<FunctionLocalState> Init(ExpressionState &state, const BoundFunctionExpression &expr,
	                                           FunctionData *bind_data) {
		if (!bind_data) {
			return nullptr;
		}
		return make_uniq<NullIfLocalState>(state, *bind_data->Cast<NullIfBindData>().collation,
		                                   expr.GetChildren()[0]->GetReturnType());
	}

	void Collate(const Vector &source, idx_t count, Vector &target) {
		input.data[0].Reference(source);
		input.SetChildCardinality(count);
		executor.ExecuteExpression(input, target);
	}

	ExpressionExecutor executor;
	DataChunk input;
	DataChunk collated;
};

unique_ptr<FunctionData> NullIfBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	auto compared_type = LogicalType::NormalizeType(BoundComparisonExpression::BindComparison(
	    context, ExpressionBinder::GetExpressionReturnType(*arguments[0]),
	    ExpressionBinder::GetExpressionReturnType(*arguments[1]), ExpressionType::COMPARE_EQUAL));
	bound_function.GetArguments()[0] = compared_type;
	bound_function.GetArguments()[1] = compared_type;
	bound_function.SetReturnType(compared_type);
	unique_ptr<Expression> collation = make_uniq<BoundReferenceExpression>(compared_type, 0U);
	if (!ExpressionBinder::PushCollation(context, collation, compared_type, CollationType::EQUALITY_COLLATIONS)) {
		return nullptr;
	}
	return make_uniq<NullIfBindData>(std::move(collation));
}

void OwnValidity(Vector &vector, idx_t count) {
	auto &validity = FlatVector::ValidityMutable(vector);
	validity.Copy(validity, count);
	switch (vector.GetType().InternalType()) {
	case PhysicalType::STRUCT:
		for (auto &child : StructVector::GetEntries(vector)) {
			OwnValidity(child, count);
		}
		break;
	case PhysicalType::ARRAY:
		OwnValidity(ArrayVector::GetChildMutable(vector), count * ArrayType::GetSize(vector.GetType()));
		break;
	default:
		break;
	}
}

void NullIfFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto count = args.size();
	auto &value = args.data[0];
	reference<const Vector> left = value;
	reference<const Vector> right = args.data[1];
	auto local_state = ExecuteFunctionState::GetFunctionState(state);
	if (local_state) {
		auto &collation = local_state->Cast<NullIfLocalState>();
		collation.collated.Reset();
		collation.Collate(args.data[0], count, collation.collated.data[0]);
		collation.Collate(args.data[1], count, collation.collated.data[1]);
		left = collation.collated.data[0];
		right = collation.collated.data[1];
	}
	sel_t equal_data[STANDARD_VECTOR_SIZE];
	SelectionVector equal(equal_data, STANDARD_VECTOR_SIZE);
	const auto equal_count = VectorOperations::Equals(left, right, nullptr, count, &equal, nullptr);
	if (equal_count == 0) {
		result.Reference(value);
		return;
	}
	if (equal_count == count) {
		ConstantVector::SetNull(result, count_t(count));
		return;
	}
	value.Flatten(count);
	result.Slice(value, 0, count);
	OwnValidity(result, count);
	const auto physical_type = value.GetType().InternalType();
	if (physical_type == PhysicalType::STRUCT || physical_type == PhysicalType::ARRAY) {
		for (idx_t i = 0; i < equal_count; i++) {
			FlatVector::SetNull(result, equal.get_index(i), true);
		}
		return;
	}
	auto &validity = FlatVector::ValidityMutable(result);
	for (idx_t i = 0; i < equal_count; i++) {
		validity.SetInvalid(equal.get_index(i));
	}
}

unique_ptr<BaseStatistics> NullIfStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto stats = input.child_stats[0].ToUnique();
	stats->Set(StatsInfo::CAN_HAVE_NULL_VALUES);
	return stats;
}

} // namespace

ScalarFunction NullIfFun::GetFunction() {
	ScalarFunction fun(NullIfFun::Name, {}, LogicalType::ANY, NullIfFunction, NullIfBind, NullIfStats,
	                   NullIfLocalState::Init);
	fun.GetSignature().AddParameter("a", LogicalType::ANY);
	fun.GetSignature().AddParameter("b", LogicalType::ANY);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

} // namespace duckdb
