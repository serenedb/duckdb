#include "duckdb/parser/parsed_data/alter_subscription_info.hpp"

#include <algorithm>

#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

AlterSubscriptionInfo::AlterSubscriptionInfo(AlterSubscriptionType alter_subscription_type_p, Identifier name)
    : AlterInfo(AlterType::ALTER_SUBSCRIPTION, QualifiedName(std::move(name)), OnEntryNotFound::THROW_EXCEPTION),
      alter_subscription_type(alter_subscription_type_p) {
}

AlterSubscriptionInfo::AlterSubscriptionInfo()
    : AlterInfo(AlterType::ALTER_SUBSCRIPTION), alter_subscription_type(AlterSubscriptionType::ENABLE) {
}

unique_ptr<AlterInfo> AlterSubscriptionInfo::Copy() const {
	auto result = make_uniq<AlterSubscriptionInfo>(alter_subscription_type, GetQualifiedName().Name());
	result->SetQualifiedName(GetQualifiedName());
	result->connection = connection;
	result->publications = publications;
	result->options = options;
	result->new_name = new_name;
	return std::move(result);
}

static string SubscriptionOptions(const case_insensitive_map_t<Value> &options) {
	vector<string> items;
	for (auto &option : options) {
		items.push_back(option.first + " = " + option.second.ToSQLString());
	}
	std::sort(items.begin(), items.end());
	return "(" + StringUtil::Join(items, ", ") + ")";
}

string AlterSubscriptionInfo::ToString() const {
	string result = "ALTER SUBSCRIPTION " + SQLIdentifier(GetQualifiedName().Name());
	string publication_list;
	for (auto &publication : publications) {
		publication_list += (publication_list.empty() ? " " : ", ") + KeywordHelper::WriteOptionallyQuoted(publication);
	}
	const auto with = options.empty() ? string() : " WITH " + SubscriptionOptions(options);
	switch (alter_subscription_type) {
	case AlterSubscriptionType::ENABLE:
		result += " ENABLE";
		break;
	case AlterSubscriptionType::DISABLE:
		result += " DISABLE";
		break;
	case AlterSubscriptionType::SET_CONNECTION:
		result += " CONNECTION " + KeywordHelper::WriteQuoted(connection, '\'');
		break;
	case AlterSubscriptionType::SET_PUBLICATION:
		result += " SET PUBLICATION" + publication_list + with;
		break;
	case AlterSubscriptionType::ADD_PUBLICATION:
		result += " ADD PUBLICATION" + publication_list + with;
		break;
	case AlterSubscriptionType::DROP_PUBLICATION:
		result += " DROP PUBLICATION" + publication_list + with;
		break;
	case AlterSubscriptionType::REFRESH_PUBLICATION:
		result += " REFRESH PUBLICATION" + with;
		break;
	case AlterSubscriptionType::SKIP:
		result += " SKIP " + SubscriptionOptions(options);
		break;
	case AlterSubscriptionType::RENAME:
		result += " RENAME TO " + SQLIdentifier(new_name);
		break;
	case AlterSubscriptionType::SET_OPTIONS:
		result += " SET " + SubscriptionOptions(options);
		break;
	}
	return result + ";";
}

} // namespace duckdb
