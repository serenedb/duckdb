#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/function/lambda_functions.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"

namespace duckdb {

namespace {

static void PropagateLambdaProperties(BoundScalarFunction &function, const Expression &lambda_expr) {
	if (lambda_expr.IsVolatile()) {
		function.SetVolatile();
	}
	if (lambda_expr.CanThrow()) {
		function.SetFallible();
	}
}

struct LambdaInvokeData final : public LambdaFunctionData {
	shared_ptr<Expression> lambda_expr;

	explicit LambdaInvokeData(shared_ptr<Expression> lambda_expr_p) : lambda_expr(std::move(lambda_expr_p)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<LambdaInvokeData>(lambda_expr);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<LambdaInvokeData>();
		return LambdaExpressionEquals(lambda_expr, other.lambda_expr);
	}

	//! Writes nothing: the lambda is serialized as the first child
	static void Serialize(Serializer &, const optional_ptr<FunctionData>, const BoundScalarFunction &) {
	}

	//! Deserializes a lambda function's bind data
	static unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundScalarFunction &function) {
		auto &children = deserializer.Get<const const_expression_list_t &>();
		if (children.empty() || children[0].get().GetExpressionClass() != ExpressionClass::BOUND_LAMBDA) {
			throw SerializationException("invoke was serialized without its lambda");
		}
		auto &bound_lambda_expr = children[0].get().Cast<BoundLambdaExpression>();
		PropagateLambdaProperties(function, *bound_lambda_expr.LambdaExpr());
		return make_uniq<LambdaInvokeData>(bound_lambda_expr.SharedLambdaExpr());
	}

	optional_ptr<const Expression> GetLambdaExpression() const override {
		return lambda_expr.get();
	}

	void SetLambdaExpression(shared_ptr<Expression> lambda_expr_p) override {
		lambda_expr = std::move(lambda_expr_p);
	}
};

struct LambdaInvokeState final : public FunctionLocalState {
	unique_ptr<ExpressionExecutor> executor;
	DataChunk input_chunk;
	idx_t parameter_count;

	LambdaInvokeState(unique_ptr<ExpressionExecutor> executor_p, const vector<LogicalType> &input_types,
	                  const idx_t parameter_count_p)
	    : executor(std::move(executor_p)), parameter_count(parameter_count_p) {
		input_chunk.InitializeEmpty(input_types);
	}

	static unique_ptr<FunctionLocalState> Init(ExpressionState &state, const BoundFunctionExpression &expr,
	                                           FunctionData *) {
		auto &bound_lambda_expr = expr.GetChildren()[0]->Cast<BoundLambdaExpression>();
		const auto parameter_count = bound_lambda_expr.ParameterCount();
		D_ASSERT(parameter_count < expr.GetChildren().size());

		// children[0] is the lambda placeholder itself, the parameters follow it. The lambda body refers
		// to its parameters in reverse order, so the input chunk lists them reversed
		vector<LogicalType> input_types;
		input_types.reserve(expr.GetChildren().size() - 1);
		for (idx_t i = 0; i < parameter_count; i++) {
			input_types.push_back(expr.GetChildren()[parameter_count - i]->GetReturnType());
		}
		for (idx_t i = parameter_count + 1; i < expr.GetChildren().size(); i++) {
			input_types.push_back(expr.GetChildren()[i]->GetReturnType());
		}

		auto executor = make_uniq<ExpressionExecutor>(state, *bound_lambda_expr.LambdaExpr());
		return make_uniq<LambdaInvokeState>(std::move(executor), input_types, parameter_count);
	}
};

void LambdaInvokeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<LambdaInvokeState>();
	for (idx_t i = 0; i < lstate.parameter_count; i++) {
		lstate.input_chunk.data[i].Reference(args.data[lstate.parameter_count - i]);
	}
	for (idx_t i = lstate.parameter_count + 1; i < args.ColumnCount(); i++) {
		lstate.input_chunk.data[i - 1].Reference(args.data[i]);
	}
	lstate.input_chunk.SetChildCardinality(args.size());
	lstate.executor->ExecuteExpression(lstate.input_chunk, result);
}

unique_ptr<FunctionData> LambdaInvokeBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	// the list column and the bound lambda expression
	if (arguments[0]->GetExpressionClass() != ExpressionClass::BOUND_LAMBDA) {
		throw BinderException("Invalid lambda expression passed to 'invoke' function.");
	}

	auto &bound_lambda_expr = arguments[0]->Cast<BoundLambdaExpression>();
	if (bound_lambda_expr.ParameterCount() != arguments.size() - 1) {
		throw BinderException("The number of lambda parameters does not match the number of arguments passed to the "
		                      "'invoke' function, expected %d, got %d.",
		                      bound_lambda_expr.ParameterCount(), arguments.size() - 1);
	}

	bound_function.SetReturnType(bound_lambda_expr.LambdaExpr()->GetReturnType());
	PropagateLambdaProperties(bound_function, *bound_lambda_expr.LambdaExpr());

	return make_uniq<LambdaInvokeData>(bound_lambda_expr.SharedLambdaExpr());
}

LogicalType LambdaInvokeBindParameters(ClientContext &context, const vector<LogicalType> &function_child_types,
                                       const idx_t parameter_idx, optional_ptr<BindLambdaContext> bind_lambda_context) {
	// The first parameter is always the lambda
	auto child_idx = parameter_idx + 1;
	if (child_idx >= function_child_types.size()) {
		throw BinderException("The number of lambda parameters does not match the number of arguments passed to the "
		                      "'invoke' function, expected at least %d, got %d.",
		                      parameter_idx + 1, function_child_types.size() - 1);
	}
	return function_child_types[child_idx];
}

} // namespace

ScalarFunction InvokeFun::GetFunction() {
	ScalarFunction fun("invoke", {}, LogicalType::ANY, LambdaInvokeFunction);
	fun.GetSignature().AddParameter("lambda", LogicalType::LAMBDA).AddParameter("arg1", LogicalType::ANY);
	fun.SetBindCallback(LambdaInvokeBind);
	fun.GetSignature().AddArgs("args", LogicalType::ANY);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetBindLambdaCallback(LambdaInvokeBindParameters);
	fun.SetInitStateCallback(LambdaInvokeState::Init);
	fun.SetSerializeCallback(LambdaInvokeData::Serialize);
	fun.SetDeserializeCallback(LambdaInvokeData::Deserialize);
	// the lambda expression that is executed for every row can throw
	fun.SetFallible();
	return fun;
}

} // namespace duckdb
