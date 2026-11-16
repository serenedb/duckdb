#include "duckdb/planner/operator/logical_alter.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"

namespace duckdb {

LogicalAlter::LogicalAlter(vector<unique_ptr<AlterInfo>> infos_p)
    : LogicalOperator(LogicalOperatorType::LOGICAL_ALTER), infos(std::move(infos_p)) {
}

LogicalAlter::~LogicalAlter() {
}

void LogicalAlter::Serialize(Serializer &serializer) const {
	LogicalOperator::Serialize(serializer);
	serializer.WritePropertyWithDefault(201, "infos", infos);
}

unique_ptr<LogicalOperator> LogicalAlter::Deserialize(Deserializer &deserializer) {
	vector<unique_ptr<AlterInfo>> infos;
	for (auto &info : deserializer.ReadPropertyWithDefault<vector<unique_ptr<ParseInfo>>>(201, "infos")) {
		infos.push_back(unique_ptr_cast<ParseInfo, AlterInfo>(std::move(info)));
	}
	return make_uniq<LogicalAlter>(std::move(infos));
}

idx_t LogicalAlter::EstimateCardinality(ClientContext &context) {
	return 1;
}

void LogicalAlter::ResolveTypes() {
	types.emplace_back(LogicalType::BOOLEAN);
}

} // namespace duckdb
