#include "core_functions/scalar/string_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/string_format.hpp"
#include "utf8proc_wrapper.hpp"

namespace duckdb {

struct FMTPrintf {
	static void OP(string &result, std::string_view format_str, const vector<FormatArgument> &format_args) {
		StringFormat::Printf(result, format_str, format_args);
	}
};

struct FMTFormat {
	static void OP(string &result, std::string_view format_str, const vector<FormatArgument> &format_args) {
		StringFormat::Format(result, format_str, format_args);
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

struct PrintfArgumentColumn {
	explicit PrintfArgumentColumn(const Vector &input) : type(input.GetType().id()) {
		input.ToUnifiedFormat(format);
	}

	template <class T>
	const T &Get(idx_t idx) const {
		return UnifiedVectorFormat::GetData<T>(format)[idx];
	}

	//! Appends the argument of the row, returns false if it is NULL
	bool Append(idx_t row, vector<FormatArgument> &result) const {
		const auto idx = format.sel->get_index(row);
		if (!format.validity.RowIsValid(idx)) {
			return false;
		}
		switch (type) {
		case LogicalTypeId::BOOLEAN:
			result.emplace_back(Get<bool>(idx));
			break;
		case LogicalTypeId::TINYINT:
			result.emplace_back(static_cast<int64_t>(Get<int8_t>(idx)));
			break;
		case LogicalTypeId::SMALLINT:
			result.emplace_back(static_cast<int64_t>(Get<int16_t>(idx)));
			break;
		case LogicalTypeId::INTEGER:
			result.emplace_back(static_cast<int64_t>(Get<int32_t>(idx)));
			break;
		case LogicalTypeId::BIGINT:
			result.emplace_back(Get<int64_t>(idx));
			break;
		case LogicalTypeId::UBIGINT:
			result.emplace_back(Get<uint64_t>(idx));
			break;
		case LogicalTypeId::FLOAT:
			result.emplace_back(static_cast<double>(Get<float>(idx)));
			break;
		case LogicalTypeId::HUGEINT:
			result.emplace_back(Get<hugeint_t>(idx));
			break;
		case LogicalTypeId::UHUGEINT:
			result.emplace_back(Get<uhugeint_t>(idx));
			break;
		case LogicalTypeId::DOUBLE:
			result.emplace_back(Get<double>(idx));
			break;
		case LogicalTypeId::VARCHAR: {
			const auto &input = Get<string_t>(idx);
			result.emplace_back(std::string_view(input.GetData(), input.GetSize()));
			break;
		}
		default:
			throw InternalException("Unexpected type for printf format");
		}
		return true;
	}

	LogicalTypeId type;
	UnifiedVectorFormat format;
};

template <class FORMAT_FUN>
static void PrintfFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	idx_t count = args.size();

	auto format_data = args.data[0].Values<string_t>();
	vector<PrintfArgumentColumn> columns;
	columns.reserve(args.ColumnCount() - 1);
	for (idx_t i = 1; i < args.ColumnCount(); i++) {
		columns.emplace_back(args.data[i]);
	}

	vector<FormatArgument> row_args;
	row_args.reserve(columns.size());
	string formatted;
	auto result_data = FlatVector::Writer<string_t>(result, count);
	for (idx_t idx = 0; idx < count; idx++) {
		auto entry = format_data[idx];
		row_args.clear();
		bool has_null = !entry.IsValid();
		for (idx_t col = 0; col < columns.size() && !has_null; col++) {
			has_null = !columns[col].Append(idx, row_args);
		}
		if (has_null) {
			// either format string or one of the input arguments is NULL
			result_data.WriteNull();
			continue;
		}

		const auto &format_string = entry.GetValue();
		const std::string_view format_view(format_string.GetData(), format_string.GetSize());
		FORMAT_FUN::OP(formatted, format_view, row_args);
		if (!Utf8Proc::IsValid(formatted.c_str(), formatted.size())) {
			throw InvalidInputException("Invalid UTF8 produced by format string \"%s\" - note that %%c writes a "
			                            "single byte, use chr(...) to write a Unicode code point",
			                            string(format_view));
		}
		result_data.WriteValue(string_t(formatted.data(), UnsafeNumericCast<uint32_t>(formatted.size())));
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
