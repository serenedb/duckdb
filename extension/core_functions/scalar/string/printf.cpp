#include "core_functions/scalar/string_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/string_format.hpp"
#include "utf8proc_wrapper.hpp"

namespace duckdb {

struct FMTPrintf {
	static string OP(const string &format_str, const vector<FormatArgument> &format_args) {
		return StringFormat::Printf(format_str, format_args);
	}
};

struct FMTFormat {
	static string OP(const string &format_str, const vector<FormatArgument> &format_args) {
		return StringFormat::Format(format_str, format_args);
	}
};

static unique_ptr<FunctionData> BindPrintfFunction(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	for (idx_t i = 1; i < arguments.size(); i++) {
		switch (arguments[i]->GetReturnType().id()) {
		case LogicalTypeId::BOOLEAN:
			bound_function.GetArguments()[i] = LogicalType::BOOLEAN;
			break;
		case LogicalTypeId::TINYINT:
		case LogicalTypeId::SMALLINT:
		case LogicalTypeId::INTEGER:
		case LogicalTypeId::BIGINT:
			bound_function.GetArguments()[i] = LogicalType::BIGINT;
			break;
		case LogicalTypeId::UTINYINT:
		case LogicalTypeId::USMALLINT:
		case LogicalTypeId::UINTEGER:
		case LogicalTypeId::UBIGINT:
			bound_function.GetArguments()[i] = LogicalType::UBIGINT;
			break;
		case LogicalTypeId::HUGEINT:
			bound_function.GetArguments()[i] = LogicalType::HUGEINT;
			break;
		case LogicalTypeId::UHUGEINT:
			bound_function.GetArguments()[i] = LogicalType::UHUGEINT;
			break;
		case LogicalTypeId::FLOAT:
		case LogicalTypeId::DOUBLE:
			bound_function.GetArguments()[i] = LogicalType::DOUBLE;
			break;
		case LogicalTypeId::VARCHAR:
			bound_function.GetArguments()[i] = LogicalType::VARCHAR;
			break;
		case LogicalTypeId::DECIMAL:
			// decimal type: add cast to double
			bound_function.GetArguments()[i] = LogicalType::DOUBLE;
			break;
		case LogicalTypeId::UNKNOWN:
			// parameter: accept any input and rebind later
			bound_function.GetArguments()[i] = LogicalType::ANY;
			break;
		default:
			// all other types: add cast to string
			bound_function.GetArguments()[i] = LogicalType::VARCHAR;
			break;
		}
	}
	return nullptr;
}

template <class ARG>
struct StandardConstructArgument {
	template <class T>
	static void ConstructArgument(const T &input, vector<FormatArgument> &result) {
		result.emplace_back(static_cast<ARG>(input));
	}
};

struct StringConstructArgument {
	template <class T>
	static void ConstructArgument(const T &input, vector<FormatArgument> &result) {
		result.emplace_back(std::string_view(input.GetData(), input.GetSize()));
	}
};

template <class T, class OP>
static void ConvertArguments(const Vector &input, idx_t arg_idx, vector<vector<FormatArgument>> &result_args) {
	auto result = input.Values<T>();
	for (idx_t i = 0; i < input.size(); i++) {
		auto &args = result_args[i];
		if (args.size() != arg_idx - 1) {
			// this entry has a NULL as one of the parameters
			continue;
		}
		auto entry = result[i];
		if (!entry.IsValid()) {
			args.clear();
			continue;
		}
		OP::ConstructArgument(entry.GetValue(), args);
	}
}

template <class FORMAT_FUN>
static void PrintfFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	idx_t count = args.size();

	// convert all format arguments
	vector<vector<FormatArgument>> format_args;
	format_args.resize(count);

	auto format_data = args.data[0].Values<string_t>();

	for (idx_t i = 1; i < args.ColumnCount(); i++) {
		const auto &col = args.data[i];
		switch (col.GetType().id()) {
		case LogicalTypeId::BOOLEAN:
			ConvertArguments<bool, StandardConstructArgument<bool>>(col, i, format_args);
			break;
		case LogicalTypeId::TINYINT:
			ConvertArguments<int8_t, StandardConstructArgument<int64_t>>(col, i, format_args);
			break;
		case LogicalTypeId::SMALLINT:
			ConvertArguments<int16_t, StandardConstructArgument<int64_t>>(col, i, format_args);
			break;
		case LogicalTypeId::INTEGER:
			ConvertArguments<int32_t, StandardConstructArgument<int64_t>>(col, i, format_args);
			break;
		case LogicalTypeId::BIGINT:
			ConvertArguments<int64_t, StandardConstructArgument<int64_t>>(col, i, format_args);
			break;
		case LogicalTypeId::UBIGINT:
			ConvertArguments<uint64_t, StandardConstructArgument<uint64_t>>(col, i, format_args);
			break;
		case LogicalTypeId::FLOAT:
			ConvertArguments<float, StandardConstructArgument<double>>(col, i, format_args);
			break;
		case LogicalTypeId::HUGEINT:
			ConvertArguments<hugeint_t, StandardConstructArgument<hugeint_t>>(col, i, format_args);
			break;
		case LogicalTypeId::UHUGEINT:
			ConvertArguments<uhugeint_t, StandardConstructArgument<uhugeint_t>>(col, i, format_args);
			break;
		case LogicalTypeId::DOUBLE:
			ConvertArguments<double, StandardConstructArgument<double>>(col, i, format_args);
			break;
		case LogicalTypeId::VARCHAR:
			ConvertArguments<string_t, StringConstructArgument>(col, i, format_args);
			break;
		default:
			throw InternalException("Unexpected type for printf format");
		}
	}

	// now perform the actual formatting
	auto result_data = FlatVector::Writer<string_t>(result, count);
	for (idx_t idx = 0; idx < count; idx++) {
		auto entry = format_data[idx];
		auto &current_args = format_args[idx];
		if (!entry.IsValid() || current_args.size() != args.ColumnCount() - 1) {
			// either format string or one of the input arguments is NULL
			result_data.WriteNull();
			continue;
		}

		auto format_string = entry.GetValue().GetString();

		// finally actually perform the format
		string dynamic_result = FORMAT_FUN::OP(format_string, current_args);
		if (!Utf8Proc::IsValid(dynamic_result.c_str(), dynamic_result.size())) {
			throw InvalidInputException("Invalid UTF8 produced by format string \"%s\" - note that %%c writes a "
			                            "single byte, use chr(...) to write a Unicode code point",
			                            format_string);
		}
		result_data.WriteValue(dynamic_result);
	}
}

ScalarFunction PrintfFun::GetFunction() {
	ScalarFunction printf_fun({}, LogicalType::VARCHAR, PrintfFunction<FMTPrintf>, BindPrintfFunction);
	printf_fun.GetSignature().AddParameter("format", LogicalType::VARCHAR);
	printf_fun.GetSignature().AddArgs("args", LogicalType::ANY);
	printf_fun.SetFallible();
	return printf_fun;
}

ScalarFunction FormatFun::GetFunction() {
	ScalarFunction format_fun({}, LogicalType::VARCHAR, PrintfFunction<FMTFormat>, BindPrintfFunction);
	format_fun.GetSignature().AddParameter("format", LogicalType::VARCHAR);
	format_fun.GetSignature().AddArgs("args", LogicalType::ANY);
	format_fun.SetFallible();
	return format_fun;
}

} // namespace duckdb
