#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/common/extra_type_info.hpp"

namespace duckdb {

int64_t TypeModifierAsInteger(const Value &value) {
	if (value.IsNull()) {
		throw BinderException("type modifiers must be simple constants or identifiers");
	}
	switch (value.type().id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL: {
		auto text = value.ToString();
		int64_t result;
		if (!TryCast::Operation<string_t, int64_t>(string_t(text), result, true)) {
			throw BinderException("invalid input syntax for type integer: \"%s\"", text);
		}
		return result;
	}
	default:
		throw BinderException("type modifiers must be simple constants or identifiers");
	}
}

CreateTypeInfo::CreateTypeInfo() : CreateInfo(CatalogType::TYPE_ENTRY), bind_function(nullptr) {
}
CreateTypeInfo::CreateTypeInfo(string name_p, LogicalType type_p, bind_logical_type_function_t bind_function_p)
    : CreateInfo(CatalogType::TYPE_ENTRY), type(std::move(type_p)), bind_function(bind_function_p) {
	SetTypeName(Identifier(std::move(name_p)));
}

unique_ptr<CreateInfo> CreateTypeInfo::Copy() const {
	auto result = make_uniq<CreateTypeInfo>();
	CopyProperties(*result);
	result->SetTypeName(GetTypeName());
	result->type = type;
	if (query) {
		result->query = query->Copy();
	}
	result->bind_function = bind_function;
	return std::move(result);
}

string CreateTypeInfo::ToString() const {
	string result = GetCreatePrefix("TYPE");
	result += QualifiedNameToString();
	if (type.id() == LogicalTypeId::ENUM) {
		auto &values_insert_order = EnumType::GetValuesInsertOrder(type);
		idx_t size = EnumType::GetSize(type);

		result += " AS ENUM ( ";
		for (idx_t i = 0; i < size; i++) {
			result += "'" + values_insert_order.GetValue(i).ToString() + "'";
			if (i != size - 1) {
				result += ", ";
			}
		}
		result += " );";
	} else if (type.id() == LogicalTypeId::INVALID) {
		// CREATE TYPE mood AS ENUM (SELECT 'happy')
		D_ASSERT(query);
		result += " AS ENUM (" + query->ToString() + ")";
	} else {
		result += " AS ";
		result += type.ToString();
	}
	result += ";";
	return result;
}

} // namespace duckdb
