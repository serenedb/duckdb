#include "duckdb/common/explain_value.hpp"

namespace duckdb {

static void FlattenExplainNode(const ExplainNode &node, idx_t depth, string &result) {
	if (!result.empty()) {
		result += "\n";
	}
	string indent(depth * 2, ' ');
	result += indent + node.label;
	for (auto &attr : node.attributes) {
		result += "\n" + indent + "  " + attr.first + ": " + attr.second;
	}
	for (auto &child : node.children) {
		FlattenExplainNode(child, depth + 1, result);
	}
}

string ExplainValue::ToString() const {
	if (!structured) {
		return scalar;
	}
	string result;
	FlattenExplainNode(*structured, 0, result);
	return result;
}

} // namespace duckdb
