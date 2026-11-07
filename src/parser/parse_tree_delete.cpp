#include "duckdb/common/parse_tree_delete.hpp"

#include "duckdb/common/stack_depth.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/tableref.hpp"

namespace duckdb {

namespace {

struct PendingDelete {
	void *node;
	void (*destroy)(void *node);
};

thread_local vector<PendingDelete> *pending_deletes = nullptr;

void DeleteIteratively(PendingDelete entry) noexcept {
	if (pending_deletes) {
		pending_deletes->push_back(entry);
		return;
	}
	vector<PendingDelete> pending {entry};
	pending_deletes = &pending;
	while (!pending.empty()) {
		auto next = pending.back();
		pending.pop_back();
		next.destroy(next.node);
	}
	pending_deletes = nullptr;
}

template <class NODE>
void Destroy(void *node) noexcept {
	delete static_cast<NODE *>(node);
}

template <class NODE>
void DeleteNode(NODE *node) noexcept {
	if (!StackDepth::Exceeded()) {
		delete node;
		return;
	}
	DeleteIteratively({node, Destroy<NODE>});
}

} // namespace

void DeleteParseTree(ParsedExpression *node) noexcept {
	DeleteNode(node);
}

void DeleteParseTree(QueryNode *node) noexcept {
	DeleteNode(node);
}

void DeleteParseTree(TableRef *node) noexcept {
	DeleteNode(node);
}

} // namespace duckdb
