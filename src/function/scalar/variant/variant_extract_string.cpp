#include "duckdb/common/types/variant.hpp"
#include "duckdb/function/cast/variant/variant_json_writer.hpp"
#include "duckdb/function/scalar/variant_functions.hpp"
#include "duckdb/function/scalar/variant_path_function.hpp"

namespace duckdb {

class VariantStringWriter {
public:
	explicit VariantStringWriter(VariantJSONWriter &json_writer, string &error)
	    : error(error), json_writer(json_writer) {
	}

	void operator()(const optional<VariantNode> &node, VectorWriter<string_t> &string_writer) const {
		if (!node || node->IsNull() || node->GetTypeId() == VariantLogicalType::VARIANT_NULL) {
			string_writer.WriteNull();
			return;
		}

		const auto type_id = node->GetTypeId();
		if (type_id == VariantLogicalType::VARCHAR) {
			string_writer.WriteValue(node->GetString());
			return;
		}

		const auto text = json_writer.WriteText(*node, error);
		if (!text) {
			throw SerializationException(error.c_str());
		}

		string_writer.WriteValue(*text);
	}

private:
	string &error;
	VariantJSONWriter &json_writer;
};

static void VariantExtractStringFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	string error;
	VariantJSONWriter json_writer;
	VariantPathFunction::Execute<string_t>(input, state, result, VariantStringWriter(json_writer, error));
}

ScalarFunctionSet VariantExtractStringFun::GetFunctions() {
	return VariantPathFunction::CreateFunctionSet("variant_extract_string", VariantExtractStringFunction,
	                                              LogicalType::VARCHAR, false);
}

} // namespace duckdb
