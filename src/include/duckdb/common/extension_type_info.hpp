#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

class Serializer;
class Deserializer;

struct LogicalTypeModifier {
public:
	explicit LogicalTypeModifier(Value value_p) : value(std::move(value_p)) {
	}
	string ToString() const {
		return label.empty() ? value.ToString() : label;
	}

public:
	Value value;
	string label;

	void Serialize(Serializer &serializer) const;
	static LogicalTypeModifier Deserialize(Deserializer &source);
};

struct ExtensionTypeInfo {
	static constexpr const char *CATALOG_OID_PROPERTY = "catalog_oid";
	static constexpr const char *DOMAIN_PROPERTY = "is_domain";
	static constexpr const char *BASE_OID_PROPERTY = "base_oid";

	vector<LogicalTypeModifier> modifiers;
	unordered_map<string, Value> properties;

public:
	void Serialize(Serializer &serializer) const;
	static unique_ptr<ExtensionTypeInfo> Deserialize(Deserializer &source);
	static bool Equals(optional_ptr<ExtensionTypeInfo> rhs, optional_ptr<ExtensionTypeInfo> lhs);
	static bool IsDomain(optional_ptr<const ExtensionTypeInfo> type_info);
};

} // namespace duckdb
