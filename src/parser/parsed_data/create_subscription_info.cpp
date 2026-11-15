#include "duckdb/parser/parsed_data/create_subscription_info.hpp"

#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

CreateSubscriptionInfo::CreateSubscriptionInfo()
    : CreateInfo(CatalogType::SUBSCRIPTION_ENTRY, Identifier::InvalidSchema()) {
}

unique_ptr<CreateInfo> CreateSubscriptionInfo::Copy() const {
	auto result = make_uniq<CreateSubscriptionInfo>();
	CopyProperties(*result);
	result->conninfo = conninfo;
	result->publications = publications;
	result->slot_name = slot_name;
	result->enabled = enabled;
	result->binary = binary;
	result->copy_data = copy_data;
	result->create_slot = create_slot;
	result->disable_on_error = disable_on_error;
	result->password_required = password_required;
	result->run_as_owner = run_as_owner;
	result->failover = failover;
	result->origin = origin;
	result->synchronous_commit = synchronous_commit;
	result->streaming = streaming;
	result->relations = relations;
	result->remote_lsn = remote_lsn;
	result->skip_lsn = skip_lsn;
	result->options = options;
	return std::move(result);
}

string CreateSubscriptionInfo::ToString() const {
	string result = "CREATE SUBSCRIPTION " + SQLIdentifier::ToString(GetQualifiedName().Name()) + " CONNECTION " +
	                SQLString::ToString(conninfo) + " PUBLICATION " +
	                StringUtil::Join(publications, ", ",
	                                 [](const string &publication) { return SQLIdentifier::ToString(publication); });
	if (!options.empty()) {
		result += " WITH " + RenderSubscriptionOptions(options);
	}
	return result + ";";
}

} // namespace duckdb
