#include "core_functions/scalar/generic_functions.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"

namespace duckdb {

namespace {
struct CurrentSettingBindData : public FunctionData {
	explicit CurrentSettingBindData(Value value_p) : value(std::move(value_p)) {
	}

	Value value;

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<CurrentSettingBindData>(value);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<CurrentSettingBindData>();
		return Value::NotDistinctFrom(value, other.value);
	}
};

void CurrentSettingFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<CurrentSettingBindData>();
	result.Reference(info.value, count_t(args.size()));
}

void CurrentSettingDynamic(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto names = args.data[0].Values<string_t>();
	auto writer = FlatVector::Writer<string_t>(result, args.size());
	for (auto entry : names) {
		if (!entry.IsValid()) {
			writer.WriteNull();
			continue;
		}
		Identifier key(entry.GetValue().GetString());
		Value val;
		if (!context.TryGetCurrentSetting(key, val)) {
			Catalog::AutoloadExtensionByConfigName(context, key);
			if (!context.TryGetCurrentSetting(key, val)) {
				throw InvalidInputException("unrecognized configuration parameter \"%s\"", key);
			}
		}
		val = Settings::FormatDisplayValue(context, val);
		if (val.IsNull()) {
			writer.WriteNull();
			continue;
		}
		auto text = val.ToString();
		writer.WriteValue(string_t(text));
	}
}

unique_ptr<FunctionData> CurrentSettingBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &key_child = input.GetArguments()[0];
	if (!key_child->HasParameter() && !key_child->IsFoldable()) {
		bound_function.SetFunctionCallback(CurrentSettingDynamic);
		bound_function.SetReturnType(LogicalType::VARCHAR);
		return nullptr;
	}

	auto key_val = input.GetNonNullConstant(0);
	auto key = key_val.GetValue<Identifier>();
	if (key.empty()) {
		throw ParserException("Key name for current_setting must not be empty");
	}

	Value val;
	if (!context.TryGetCurrentSetting(key, val)) {
		auto extension_name = Catalog::AutoloadExtensionByConfigName(context, key);
		// If autoloader didn't throw, the config is now available
		context.TryGetCurrentSetting(key, val);
	}

	val = Settings::FormatDisplayValue(context, val);
	bound_function.SetReturnType(val.type());
	return make_uniq<CurrentSettingBindData>(val);
}

} // namespace

ScalarFunction CurrentSettingFun::GetFunction() {
	auto fun = ScalarFunction({}, LogicalType::ANY, CurrentSettingFunction, CurrentSettingBind);
	fun.GetSignature().AddParameter("setting_name", LogicalType::VARCHAR);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

} // namespace duckdb
