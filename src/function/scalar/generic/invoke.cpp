#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/function/lambda_functions.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

#include <absl/cleanup/cleanup.h>

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

void MarkUnconditionalReads(const Expression &expr, vector<bool> &read) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_REF: {
		const auto index = expr.Cast<BoundReferenceExpression>().Index();
		D_ASSERT(index < read.size());
		read[index] = true;
		return;
	}
	case ExpressionClass::BOUND_CASE: {
		auto &checks = expr.Cast<BoundCaseExpression>().CaseChecks();
		if (!checks.empty()) {
			MarkUnconditionalReads(*checks[0].when_expr, read);
		}
		return;
	}
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &children = expr.Cast<BoundConjunctionExpression>().GetChildren();
		if (!children.empty()) {
			MarkUnconditionalReads(*children[0], read);
		}
		return;
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &children = expr.Cast<BoundOperatorExpression>().GetChildren();
		if (expr.GetExpressionType() != ExpressionType::OPERATOR_TRY && !children.empty()) {
			MarkUnconditionalReads(*children[0], read);
		}
		return;
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &function = expr.Cast<BoundFunctionExpression>();
		if (function.Function().GetLazyArguments()) {
			return;
		}
		auto &children = function.GetChildren();
		const auto eager =
		    function.Function().HasBindLambdaCallback() ? MinValue<idx_t>(1, children.size()) : children.size();
		for (idx_t i = 0; i < eager; i++) {
			MarkUnconditionalReads(*children[i], read);
		}
		return;
	}
	default:
		return;
	}
}

class InvokeArguments final : public LazyColumns {
public:
	InvokeArguments(ExpressionState &state_p, const BoundFunctionExpression &expr, idx_t parameter_count)
	    : state(state_p) {
		const auto column_count = expr.GetChildren().size() - 1;
		vector<bool> read(column_count, false);
		MarkUnconditionalReads(*expr.GetChildren()[0]->Cast<BoundLambdaExpression>().LambdaExpr(), read);
		arguments.reserve(column_count);
		vector<LogicalType> types;
		types.reserve(column_count);
		for (idx_t column = 0; column < column_count; column++) {
			auto &argument = arguments.emplace_back();
			argument.child = column < parameter_count ? parameter_count - column : column + 1;
			auto &child = *expr.GetChildren()[argument.child];
			argument.strict = read[column] || child.IsVolatile();
			types.push_back(child.GetReturnType());
		}
		stores.Initialize(state.GetAllocator(), types);
		placeholders.InitializeEmpty(types);
	}

	void Reset(DataChunk &chunk, idx_t count) {
		rows = count;
		for (idx_t column = 0; column < arguments.size(); column++) {
			chunk.data[column].Reference(placeholders.data[column]);
		}
		for (idx_t column = 0; column < arguments.size(); column++) {
			auto &argument = arguments[column];
			argument.stored = 0;
			argument.complete = false;
			argument.partial = false;
			argument.owned = false;
			if (argument.strict) {
				Fill(chunk, column, nullptr, count, nullptr);
			}
		}
	}

	bool Load(DataChunk &chunk, idx_t column, const SelectionVector *sel, idx_t count, Vector &result) override {
		return Fill(chunk, column, sel, count, &result);
	}

private:
	bool Fill(DataChunk &chunk, idx_t column, const SelectionVector *sel, idx_t count, optional_ptr<Vector> result) {
		auto &argument = arguments[column];
		if (argument.complete) {
			return false;
		}
		auto &executor = *state.root.executor;
		if (count == rows && !argument.partial) {
			auto &values = state.intermediate_chunk.data[argument.child];
			executor.ExecuteArgument(state, argument.child, nullptr, rows, values);
			chunk.data[column].Reference(values);
			argument.complete = true;
			return false;
		}
		auto &values = stores.data[column];
		if (!argument.partial) {
			stores.ResetColumn(column);
			argument.partial = true;
			argument.loaded.assign(rows, false);
			argument.positions.Initialize(rows);
			for (idx_t row = 0; row < rows; row++) {
				argument.positions.set_index(row, 0);
			}
		}
		SelectionVector missing(count);
		idx_t missing_count = 0;
		for (idx_t i = 0; i < count; i++) {
			const auto row = sel ? sel->get_index(i) : i;
			if (!argument.loaded[row]) {
				argument.loaded[row] = true;
				missing.set_index(missing_count++, row);
			}
		}
		if (missing_count == 0) {
			return false;
		}
		bool computed = false;
		absl::Cleanup unmark = [&] {
			if (!computed) {
				for (idx_t i = 0; i < missing_count; i++) {
					argument.loaded[missing.get_index(i)] = false;
				}
			}
		};
		const bool requested = missing_count == count;
		if (argument.stored == 0) {
			executor.ExecuteArgument(state, argument.child, &missing, missing_count, values);
			if (requested) {
				result->Reference(values);
			}
		} else {
			if (!argument.owned) {
				Vector owned(values.GetType(), rows);
				VectorOperations::Copy(values, owned, argument.stored, 0, 0);
				values.Reference(owned);
				argument.owned = true;
			}
			Vector loaded(values.GetType(), missing_count);
			executor.ExecuteArgument(state, argument.child, &missing, missing_count, loaded);
			VectorOperations::Copy(loaded, values, missing_count, 0, argument.stored);
			FlatVector::SetSize(values, argument.stored + missing_count);
			if (requested) {
				result->Reference(loaded);
			}
		}
		for (idx_t i = 0; i < missing_count; i++) {
			argument.positions.set_index(missing.get_index(i), argument.stored + i);
		}
		argument.stored += missing_count;
		chunk.data[column].Slice(values, argument.positions, rows);
		argument.complete = argument.stored == rows;
		computed = true;
		return requested;
	}

	struct Argument {
		idx_t child = 0;
		bool strict = false;
		bool complete = false;
		bool partial = false;
		bool owned = false;
		idx_t stored = 0;
		vector<bool> loaded;
		SelectionVector positions;
	};

	ExpressionState &state;
	vector<Argument> arguments;
	DataChunk stores;
	DataChunk placeholders;
	idx_t rows = 0;
};

struct LambdaInvokeState final : public FunctionLocalState {
	unique_ptr<ExpressionExecutor> executor;
	DataChunk input_chunk;
	InvokeArguments arguments;

	LambdaInvokeState(unique_ptr<ExpressionExecutor> executor_p, const vector<LogicalType> &input_types,
	                  const idx_t parameter_count, ExpressionState &state, const BoundFunctionExpression &expr)
	    : executor(std::move(executor_p)), arguments(state, expr, parameter_count) {
		input_chunk.InitializeEmpty(input_types);
		executor->lazy_columns = &arguments;
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
		return make_uniq<LambdaInvokeState>(std::move(executor), input_types, parameter_count, state, expr);
	}
};

void LambdaInvokeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<LambdaInvokeState>();
	const auto count = args.size();
	lstate.arguments.Reset(lstate.input_chunk, count);
	lstate.input_chunk.SetChildCardinality(count);
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
	fun.SetLazyArguments(true);
	return fun;
}

} // namespace duckdb
