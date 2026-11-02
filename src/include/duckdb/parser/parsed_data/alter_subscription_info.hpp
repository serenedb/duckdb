//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/alter_subscription_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"

namespace duckdb {

enum class AlterSubscriptionType : uint8_t {
	ENABLE = 0,
	DISABLE = 1,
	SET_CONNECTION = 2,
	SET_PUBLICATION = 3,
	ADD_PUBLICATION = 4,
	DROP_PUBLICATION = 5,
	REFRESH_PUBLICATION = 6,
	SKIP = 7,
	RENAME = 8,
	SET_OPTIONS = 9
};

struct AlterSubscriptionInfo : public AlterInfo {
	AlterSubscriptionInfo(AlterSubscriptionType alter_subscription_type, Identifier name);
	~AlterSubscriptionInfo() override = default;

	AlterSubscriptionType alter_subscription_type;
	string connection;
	vector<string> publications;
	case_insensitive_map_t<Value> options;
	Identifier new_name;

public:
	CatalogType GetCatalogType() const override {
		return CatalogType::SUBSCRIPTION_ENTRY;
	}
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterInfo> Deserialize(Deserializer &deserializer);

private:
	AlterSubscriptionInfo();
};

} // namespace duckdb
