#include "duckdb/function/table/summary.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

// this function makes not that much sense on its own but is a demo for table-parameter table-producing functions

namespace duckdb {

static unique_ptr<FunctionData> SummaryFunctionBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<Identifier> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("summary");

	for (idx_t i = 0; i < input.input_table_types.size(); i++) {
		return_types.push_back(input.input_table_types[i]);
		names.push_back(input.input_table_names[i]);
	}

	return make_uniq<TableFunctionData>();
}

static OperatorResultType SummaryFunction(ExecutionContext &context, TableFunctionInput &data_p, DataChunk &input,
                                          DataChunk &output) {
	vector<Vector> text_columns;
	vector<VectorIterator<string_t>> texts;
	text_columns.reserve(input.ColumnCount());
	texts.reserve(input.ColumnCount());
	for (auto &column : input.data) {
		text_columns.emplace_back(LogicalType::VARCHAR, input.size());
		VectorOperations::DefaultCast(column, text_columns.back(), input.size());
		texts.emplace_back(text_columns.back());
	}
	auto writer = FlatVector::Writer<string_t>(output.data[0], input.size());
	string summary_val;
	for (idx_t row_idx = 0; row_idx < input.size(); row_idx++) {
		summary_val = "[";
		for (idx_t col_idx = 0; col_idx < texts.size(); col_idx++) {
			auto text = texts[col_idx][row_idx];
			if (text.IsValid()) {
				summary_val += text.GetValue().GetString();
			} else {
				summary_val += "NULL";
			}
			if (col_idx + 1 < texts.size()) {
				summary_val += ", ";
			}
		}
		summary_val += "]";
		writer.WriteValue(string_t(summary_val.c_str(), UnsafeNumericCast<uint32_t>(summary_val.size())));
	}
	for (idx_t col_idx = 0; col_idx < input.ColumnCount(); col_idx++) {
		output.data[col_idx + 1].Reference(input.data[col_idx]);
	}
	return OperatorResultType::NEED_MORE_INPUT;
}

void SummaryTableFunction::RegisterFunction(BuiltinFunctions &set) {
	TableFunction summary_function("summary", FunctionSignature().AddPositionalOnly("input", LogicalType::TABLE),
	                               nullptr, SummaryFunctionBind);
	summary_function.in_out_function = SummaryFunction;
	set.AddFunction(summary_function);
}

} // namespace duckdb
