#include "duckdb/planner/operator/logical_drop.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"

namespace duckdb {

LogicalDrop::LogicalDrop(vector<unique_ptr<DropInfo>> infos_p)
    : LogicalOperator(LogicalOperatorType::LOGICAL_DROP), infos(std::move(infos_p)) {
}

LogicalDrop::~LogicalDrop() {
}

void LogicalDrop::Serialize(Serializer &serializer) const {
	LogicalOperator::Serialize(serializer);
	serializer.WritePropertyWithDefault(201, "infos", infos);
}

unique_ptr<LogicalOperator> LogicalDrop::Deserialize(Deserializer &deserializer) {
	vector<unique_ptr<DropInfo>> infos;
	for (auto &info : deserializer.ReadPropertyWithDefault<vector<unique_ptr<ParseInfo>>>(201, "infos")) {
		infos.push_back(unique_ptr_cast<ParseInfo, DropInfo>(std::move(info)));
	}
	return make_uniq<LogicalDrop>(std::move(infos));
}

idx_t LogicalDrop::EstimateCardinality(ClientContext &context) {
	return 1;
}

void LogicalDrop::ResolveTypes() {
	types.emplace_back(LogicalType::BOOLEAN);
}

} // namespace duckdb
