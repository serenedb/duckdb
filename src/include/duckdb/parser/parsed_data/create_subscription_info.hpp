//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/create_subscription_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/create_info.hpp"

namespace duckdb {

struct SubscriptionRelation {
	string schema;
	string table;
	uint8_t state = 'i';
	uint64_t lsn = 0;

	bool operator==(const SubscriptionRelation &other) const {
		return schema == other.schema && table == other.table && state == other.state && lsn == other.lsn;
	}

	void Serialize(Serializer &serializer) const;
	static SubscriptionRelation Deserialize(Deserializer &deserializer);
};

struct CreateSubscriptionInfo : public CreateInfo {
	CreateSubscriptionInfo();

	string conninfo;
	vector<string> publications;
	string slot_name;
	bool enabled = true;
	bool binary = false;
	bool copy_data = true;
	bool create_slot = true;
	bool disable_on_error = false;
	bool password_required = true;
	bool run_as_owner = false;
	bool failover = false;
	string origin = "any";
	string synchronous_commit = "off";
	string streaming = "off";
	vector<SubscriptionRelation> relations;
	uint64_t remote_lsn = 0;
	uint64_t skip_lsn = 0;

public:
	DUCKDB_API void Serialize(Serializer &serializer) const override;
	DUCKDB_API static unique_ptr<CreateInfo> Deserialize(Deserializer &deserializer);

	unique_ptr<CreateInfo> Copy() const override;
	string ToString() const override;
};

} // namespace duckdb
