#include "duckdb/formats/value_parsers.hpp"

namespace duckdb {

bool HasValueParser(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::DECIMAL:
		return true;
	default:
		return false;
	}
}

bool ValueParses(const LogicalType &type, const char *value, idx_t size, const ValueParseContext &context) {
	return WithValueParser(type, context, [&](auto parsed, auto parse) {
		typename decltype(parsed)::type out;
		return parse(value, size, out);
	});
}

} // namespace duckdb
