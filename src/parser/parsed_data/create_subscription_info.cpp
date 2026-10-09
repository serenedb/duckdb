#include "duckdb/parser/parsed_data/create_subscription_info.hpp"

#include "duckdb/parser/keyword_helper.hpp"

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
	return std::move(result);
}

string CreateSubscriptionInfo::ToString() const {
	string result = "CREATE SUBSCRIPTION ";
	result += KeywordHelper::WriteOptionallyQuoted(GetQualifiedName().Name().GetIdentifierName());
	result += " CONNECTION " + KeywordHelper::WriteQuoted(conninfo, '\'');
	result += " PUBLICATION ";
	for (idx_t i = 0; i < publications.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += KeywordHelper::WriteOptionallyQuoted(publications[i]);
	}
	auto boolean = [](bool value) {
		return value ? "true" : "false";
	};
	result += " WITH (enabled = ";
	result += boolean(enabled);
	result += ", slot_name = " + KeywordHelper::WriteQuoted(slot_name, '\'');
	result += ", binary = ";
	result += boolean(binary);
	result += ", disable_on_error = ";
	result += boolean(disable_on_error);
	result += ", password_required = ";
	result += boolean(password_required);
	result += ", run_as_owner = ";
	result += boolean(run_as_owner);
	result += ", failover = ";
	result += boolean(failover);
	result += ", origin = " + KeywordHelper::WriteQuoted(origin, '\'');
	result += ", synchronous_commit = " + KeywordHelper::WriteQuoted(synchronous_commit, '\'');
	result += ", streaming = " + KeywordHelper::WriteQuoted(streaming, '\'');
	result += ");";
	return result;
}

} // namespace duckdb
