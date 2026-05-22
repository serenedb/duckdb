#include "duckdb/planner/operator/logical_explain.hpp"

namespace duckdb {

LogicalExplain::LogicalExplain(unique_ptr<LogicalOperator> plan, ExplainType explain_type,
                               const ProfilerPrintFormat &format, ExplainFormatShape output_shape)
    : LogicalOperator(LogicalOperatorType::LOGICAL_EXPLAIN), explain_type(explain_type), format(format),
      output_shape(output_shape) {
	children.push_back(std::move(plan));
}

idx_t LogicalExplain::EstimateCardinality(ClientContext &context) {
	return 3;
}

bool LogicalExplain::SupportSerialization() const {
	//! Skips the serialization check in VerifyPlan
	return false;
}

void LogicalExplain::ResolveTypes() {
	if (output_shape == ExplainFormatShape::PG) {
		types = {LogicalType::VARCHAR};
	} else {
		types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
	}
}
vector<ColumnBinding> LogicalExplain::GetColumnBindings() {
	vector<ColumnBinding> result;
	TableIndex explain_tbl_idx(0);
	auto column_count = output_shape == ExplainFormatShape::PG ? 1 : 2;
	for (auto explain_col_idx : ProjectionIndex::GetIndexes(column_count)) {
		result.emplace_back(explain_tbl_idx, explain_col_idx);
	}
	return result;
}

} // namespace duckdb
