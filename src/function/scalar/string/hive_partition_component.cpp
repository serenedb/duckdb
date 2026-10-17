#include "duckdb/function/scalar/string_functions.hpp"

#include "duckdb/common/hive_partitioning.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

namespace {

void HivePartitionComponentFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto count = args.size();
	Vector value_text(LogicalType::VARCHAR, count);
	VectorOperations::DefaultCast(args.data[1], value_text, count);
	auto names = args.data[0].Values<string_t>();
	auto values = value_text.Values<string_t>();
	auto writer = FlatVector::Writer<string_t>(result, count);
	for (idx_t row = 0; row < count; row++) {
		auto name = names[row];
		auto value = values[row];
		// the name and the value are escaped the same way url_encode does
		auto component = HivePartitioning::Escape(name.IsValid() ? name.GetValue().GetString() : "NULL") + "=";
		component += value.IsValid() ? HivePartitioning::EscapeValue(value.GetValue().GetString())
		                             : HivePartitioning::DEFAULT_PARTITION_NAME;
		writer.WriteValue(string_t(component.c_str(), UnsafeNumericCast<uint32_t>(component.size())));
	}
}

} // namespace

ScalarFunction HivePartitionComponentFun::GetFunction() {
	ScalarFunction component(HivePartitionComponentFun::Name, {}, LogicalType::VARCHAR, HivePartitionComponentFunction);
	component.GetSignature().AddParameter("name", LogicalType::VARCHAR).AddParameter("value", LogicalType::ANY);
	// a NULL partition value has a directory of its own
	component.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return component;
}

} // namespace duckdb
