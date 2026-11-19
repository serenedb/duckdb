#include "duckdb/formats/value_codecs.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

namespace {

class CastDecoder final : public ValueDecoder {
public:
	explicit CastDecoder(ClientContext &context_p) : context(context_p) {
	}

	bool Decode(Vector &text, Vector &out, idx_t count, string &error) final {
		return VectorOperations::TryCast(context, text, out, count, &error, false, true);
	}

private:
	ClientContext &context;
};

} // namespace

ValueRendering ValueRenderingFromString(const string &name, const string &option) {
	const auto lowered = StringUtil::Lower(name);
	if (lowered == "duckdb") {
		return ValueRendering::NATIVE;
	}
	if (lowered == "postgres") {
		return ValueRendering::POSTGRES;
	}
	throw InvalidInputException("%s must be 'duckdb' or 'postgres', got '%s'", option, name);
}

unique_ptr<ValueDecoder> MakeValueDecoder(ClientContext &context, const LogicalType &type, ValueRendering rendering) {
	if (rendering == ValueRendering::NATIVE) {
		return make_uniq<CastDecoder>(context);
	}
	auto create = DBConfig::GetConfig(context).create_postgres_decoder;
	if (!create) {
		throw InvalidInputException("The postgres rendering needs PostgreSQL's input functions, which this database "
		                            "does not provide");
	}
	return create(context, type);
}

} // namespace duckdb
