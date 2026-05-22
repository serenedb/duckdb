//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/explain_value.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/insertion_order_preserving_map.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

//! A structured operator-info value: a labeled tree with scalar attributes per node.
//! Producers (ParamsToString / table function to_string callbacks) can attach one instead of a
//! pre-rendered string; each renderer then renders it natively (nested boxes in text, nested
//! objects in JSON) or falls back to the indented flat form from ExplainValue::ToString.
struct ExplainNode {
	explicit ExplainNode(string label = string()) : label(std::move(label)) {
	}

	string label;
	InsertionOrderPreservingMap<string> attributes;
	vector<ExplainNode> children;
};

//! A single operator-info ("extra info") value: either a plain string or a structured tree.
//! Implicitly constructible from strings so existing `result["key"] = "value"` producers work
//! unchanged. Copies are cheap - the structured tree is shared and immutable once attached.
class ExplainValue {
public:
	ExplainValue() = default;
	ExplainValue(string scalar_p) : scalar(std::move(scalar_p)) { // NOLINT: allow implicit conversion
	}
	ExplainValue(const char *scalar_p) : scalar(scalar_p) { // NOLINT: allow implicit conversion
	}
	explicit ExplainValue(ExplainNode node) : structured(make_shared_ptr<ExplainNode>(std::move(node))) {
	}

	bool IsStructured() const {
		return structured != nullptr;
	}
	const string &Scalar() const {
		return scalar;
	}
	const ExplainNode &Structured() const {
		return *structured;
	}
	bool empty() const { // NOLINT: match stl API
		return !structured && scalar.empty();
	}
	//! Flatten to an indented multi-line string (for renderers without nested-value support)
	string ToString() const;

private:
	string scalar;
	shared_ptr<ExplainNode> structured;
};

} // namespace duckdb
