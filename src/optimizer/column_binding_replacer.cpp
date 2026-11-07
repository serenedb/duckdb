#include "duckdb/optimizer/column_binding_replacer.hpp"

#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/operator/logical_cte.hpp"
#include "duckdb/planner/operator/logical_dependent_join.hpp"
#include "duckdb/planner/operator/logical_secure_view.hpp"
#include "duckdb/planner/column_binding_map.hpp"

namespace duckdb {

ReplacementBinding::ReplacementBinding(ColumnBinding old_binding, ColumnBinding new_binding)
    : old_binding(old_binding), new_binding(new_binding), replace_type(false) {
}

ReplacementBinding::ReplacementBinding(ColumnBinding old_binding, ColumnBinding new_binding, LogicalType new_type)
    : old_binding(old_binding), new_binding(new_binding), replace_type(true), new_type(std::move(new_type)) {
}

optional_ptr<const ReplacementBinding> BindingReplacementGraph::Find(ColumnBinding old_binding) const {
	auto entry = replacement_positions.find(old_binding);
	if (entry == replacement_positions.end()) {
		return nullptr;
	}
	return replacement_bindings[entry->second];
}

ReplacementBinding BindingReplacementGraph::ResolveReplacement(ColumnBinding binding) const {
	ReplacementBinding result(binding, binding);
	column_binding_set_t visited;
	while (visited.insert(result.new_binding).second) {
		auto replacement = Find(result.new_binding);
		if (!replacement) {
			return result;
		}
		result.new_binding = replacement->new_binding;
		if (replacement->replace_type) {
			result.replace_type = true;
			result.new_type = replacement->new_type;
		}
	}
	throw InternalException("Cyclic column binding replacements");
}

ColumnBinding BindingReplacementGraph::Resolve(ColumnBinding binding) const {
	return ResolveReplacement(binding).new_binding;
}

void BindingReplacementGraph::Add(ColumnBinding old_binding, ColumnBinding new_binding) {
	Add(ReplacementBinding(old_binding, new_binding));
}

bool BindingReplacementGraph::TryAdd(const ReplacementBinding &replacement) {
	auto resolved_new = ResolveReplacement(replacement.new_binding);
	if (replacement.old_binding == resolved_new.new_binding) {
		if (replacement.old_binding != replacement.new_binding) {
			return false;
		}
		return true;
	}
	auto position = replacement_positions.emplace(replacement.old_binding, replacement_bindings.size());
	if (!position.second) {
		auto &existing = replacement_bindings[position.first->second];
		if (existing.new_binding != replacement.new_binding ||
		    (existing.replace_type && replacement.replace_type && existing.new_type != replacement.new_type)) {
			return false;
		}
		if (!existing.replace_type && replacement.replace_type) {
			existing.replace_type = true;
			existing.new_type = replacement.new_type;
		}
		return true;
	}
	if (replacement.replace_type) {
		replacement_bindings.emplace_back(replacement.old_binding, replacement.new_binding, replacement.new_type);
	} else {
		replacement_bindings.emplace_back(replacement.old_binding, replacement.new_binding);
	}
	return true;
}

void BindingReplacementGraph::Add(const ReplacementBinding &replacement) {
	if (!TryAdd(replacement)) {
		throw InternalException("Conflicting or cyclic column binding replacement for %s",
		                        replacement.old_binding.ToString());
	}
}

void BindingReplacementGraph::Merge(const BindingReplacementGraph &replacements) {
	for (auto &replacement : replacements.replacement_bindings) {
		Add(replacement);
	}
}

void BindingReplacementGraph::AddTo(ColumnBindingReplacer &replacer) const {
	for (auto &replacement : replacement_bindings) {
		auto resolved = ResolveReplacement(replacement.old_binding);
		if (resolved.replace_type) {
			replacer.replacement_bindings.emplace_back(replacement.old_binding, resolved.new_binding,
			                                           resolved.new_type);
		} else {
			replacer.replacement_bindings.emplace_back(replacement.old_binding, resolved.new_binding);
		}
	}
}

ColumnBindingReplacer::ColumnBindingReplacer() {
}

void ColumnBindingReplacer::AddReplacements(const vector<ColumnBinding> &old_bindings,
                                            const vector<ColumnBinding> &new_bindings) {
	if (old_bindings.size() != new_bindings.size()) {
		throw InternalException("Column binding replacement lists must have the same size");
	}
	replacement_bindings.reserve(replacement_bindings.size() + old_bindings.size());
	for (idx_t i = 0; i < old_bindings.size(); i++) {
		if (old_bindings[i] != new_bindings[i]) {
			replacement_bindings.emplace_back(old_bindings[i], new_bindings[i]);
		}
	}
}

ColumnBindingReplacer::IndexScope::IndexScope(ColumnBindingReplacer &replacer) : replacer(replacer) {
	if (replacer.index_depth++ > 0) {
		return;
	}
	replacer.scope_lookups = 0;
	replacer.index_built = false;
}

ColumnBindingReplacer::IndexScope::~IndexScope() {
	if (--replacer.index_depth > 0) {
		return;
	}
	if (replacer.index_built) {
		replacer.first_replacement.clear();
		replacer.next_replacement.clear();
		replacer.index_built = false;
	}
}

void ColumnBindingReplacer::BuildReplacementIndex() {
	first_replacement.clear();
	first_replacement.reserve(replacement_bindings.size());
	next_replacement.assign(replacement_bindings.size(), DConstants::INVALID_INDEX);
	for (idx_t i = replacement_bindings.size(); i > 0; i--) {
		auto position = i - 1;
		auto entry = first_replacement.emplace(replacement_bindings[position].old_binding, position);
		if (!entry.second) {
			next_replacement[position] = entry.first->second;
			entry.first->second = position;
		}
	}
	index_built = true;
}

template <class F>
void ColumnBindingReplacer::ApplyReplacements(ColumnBinding binding, F &&apply) {
	static constexpr idx_t LINEAR_LOOKUPS = 8;
	if (index_depth == 0 || replacement_bindings.size() <= LINEAR_LOOKUPS || ++scope_lookups <= LINEAR_LOOKUPS) {
		for (auto &replacement : replacement_bindings) {
			if (binding == replacement.old_binding) {
				apply(replacement);
				binding = replacement.new_binding;
			}
		}
		return;
	}
	if (!index_built) {
		BuildReplacementIndex();
	}
	idx_t position = 0;
	while (true) {
		auto entry = first_replacement.find(binding);
		if (entry == first_replacement.end()) {
			return;
		}
		auto next = entry->second;
		while (next != DConstants::INVALID_INDEX && next < position) {
			next = next_replacement[next];
		}
		if (next == DConstants::INVALID_INDEX) {
			return;
		}
		auto &replacement = replacement_bindings[next];
		apply(replacement);
		binding = replacement.new_binding;
		position = next + 1;
	}
}

void ColumnBindingReplacer::VisitOperator(LogicalOperator &op) {
	if (stop_operator && stop_operator.get() == &op) {
		return;
	}
	IndexScope scope(*this);
	VisitOperatorChildren(op);
	VisitOperatorBindings(op);
}

void ColumnBindingReplacer::VisitOperatorBindings(LogicalOperator &op) {
	IndexScope scope(*this);
	if (op.type == LogicalOperatorType::LOGICAL_SECURE_VIEW) {
		auto &view = op.Cast<LogicalSecureView>();
		for (auto &binding : view.output_bindings) {
			ApplyReplacements(binding,
			                  [&](const ReplacementBinding &replacement) { binding = replacement.new_binding; });
		}
	}
	VisitOperatorExpressions(op);
}

void ColumnBindingReplacer::VisitExpression(unique_ptr<Expression> *expression) {
	IndexScope scope(*this);
	LogicalOperatorVisitor::VisitExpression(expression);
}

unique_ptr<Expression> ColumnBindingReplacer::VisitReplace(BoundColumnRefExpression &expr,
                                                           unique_ptr<Expression> *expr_ptr) {
	ApplyReplacements(expr.Binding(), [&](const ReplacementBinding &replacement) {
		expr.BindingMutable() = replacement.new_binding;
		if (replacement.replace_type) {
			expr.SetReturnType(replacement.new_type);
		}
	});
	return nullptr;
}

void CorrelatedColumnBindingReplacer::ReplaceCorrelatedColumns(CorrelatedColumns &columns) {
	for (auto &column : columns) {
		ApplyReplacements(column.binding, [&](const ReplacementBinding &replacement) {
			column.binding = replacement.new_binding;
			if (replacement.replace_type) {
				column.type = replacement.new_type;
			}
		});
	}
}

void CorrelatedColumnBindingReplacer::VisitOperatorBindings(LogicalOperator &op) {
	IndexScope scope(*this);
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_DEPENDENT_JOIN: {
		auto &dependent_join = op.Cast<LogicalDependentJoin>();
		ReplaceCorrelatedColumns(dependent_join.correlated_columns);
		break;
	}
	case LogicalOperatorType::LOGICAL_RECURSIVE_CTE:
	case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
		ReplaceCorrelatedColumns(op.Cast<LogicalCTE>().correlated_columns);
		break;
	default:
		break;
	}
	ColumnBindingReplacer::VisitOperatorBindings(op);
}

unique_ptr<Expression> CorrelatedColumnBindingReplacer::VisitReplace(BoundSubqueryExpression &expr,
                                                                     unique_ptr<Expression> *expr_ptr) {
	ReplaceCorrelatedColumns(expr.GetBinder()->correlated_columns);
	if (expr.SubqueryMutable().plan) {
		VisitOperator(*expr.SubqueryMutable().plan);
	}
	return nullptr;
}

void ColumnBindingRewrite::RemapProjectionMapStrict(vector<ProjectionIndex> &projection_map,
                                                    const vector<ColumnBinding> &original_child_bindings,
                                                    const vector<ColumnBinding> &child_bindings_before,
                                                    const vector<ColumnBinding> &child_bindings_after) {
	vector<ColumnBinding> selected_bindings;
	column_binding_map_t<ColumnBinding> selected_origins;
	auto select_binding = [&](idx_t position) {
		auto &binding = child_bindings_before[position];
		auto &origin = original_child_bindings[position];
		auto entry = selected_origins.emplace(binding, origin);
		if (entry.second || entry.first->second == origin) {
			selected_bindings.push_back(binding);
		}
	};
	if (projection_map.empty()) {
		selected_bindings.reserve(child_bindings_before.size());
		selected_origins.reserve(child_bindings_before.size());
		for (idx_t position = 0; position < child_bindings_before.size(); position++) {
			select_binding(position);
		}
	} else {
		selected_bindings.reserve(projection_map.size());
		selected_origins.reserve(projection_map.size());
		for (auto projection_index : projection_map) {
			if (projection_index.GetIndex() >= child_bindings_before.size()) {
				throw InternalException("Projection map references column %llu in a child with %llu columns",
				                        projection_index.GetIndex(), child_bindings_before.size());
			}
			select_binding(projection_index.GetIndex());
		}
	}
	if (selected_bindings == child_bindings_after) {
		projection_map.clear();
		return;
	}
	column_binding_map_t<idx_t> after_positions;
	after_positions.reserve(child_bindings_after.size());
	for (idx_t i = 0; i < child_bindings_after.size(); i++) {
		after_positions.emplace(child_bindings_after[i], i);
	}
	vector<ProjectionIndex> new_projection_map;
	new_projection_map.reserve(selected_bindings.size());
	for (auto &binding : selected_bindings) {
		auto entry = after_positions.find(binding);
		if (entry == after_positions.end()) {
			throw InternalException("Binding rewrite lost projected child binding %s (selected %s, child output %s)",
			                        binding.ToString(), LogicalOperator::ColumnBindingsToString(selected_bindings),
			                        LogicalOperator::ColumnBindingsToString(child_bindings_after));
		}
		new_projection_map.emplace_back(entry->second);
	}
	projection_map = std::move(new_projection_map);
}

void ColumnBindingRewrite::ApplyToOperatorBindings(LogicalOperator &op, const BindingReplacementGraph &replacements) {
	if (replacements.Empty()) {
		return;
	}
	CorrelatedColumnBindingReplacer replacer;
	replacements.AddTo(replacer);
	replacer.VisitOperatorBindings(op);
}

namespace {

class OutputBindingResolver {
public:
	OutputBindingResolver(const column_binding_set_t &new_bindings, const BindingReplacementGraph &replacements)
	    : new_bindings(new_bindings), replacements(replacements) {
	}

	bool TryResolve(ColumnBinding binding, ReplacementBinding &result) {
		auto &resolution = Resolve(binding);
		if (resolution.replace_type) {
			result = ReplacementBinding(binding, resolution.target, resolution.new_type);
		} else {
			result = ReplacementBinding(binding, resolution.target);
		}
		return resolution.resolved;
	}

private:
	struct Resolution {
		bool resolved = false;
		ColumnBinding target;
		bool replace_type = false;
		LogicalType new_type;
	};

	const Resolution &Resolve(ColumnBinding binding) {
		vector<ColumnBinding> path;
		vector<reference<const ReplacementBinding>> steps;
		column_binding_set_t on_path;
		Resolution tail;
		auto current = binding;
		while (true) {
			if (new_bindings.find(current) != new_bindings.end()) {
				tail.resolved = true;
				tail.target = current;
				break;
			}
			auto memoized = resolutions.find(current);
			if (memoized != resolutions.end()) {
				tail = memoized->second;
				break;
			}
			if (!on_path.insert(current).second) {
				throw InternalException("Cyclic column binding replacements");
			}
			auto next = replacements.Find(current);
			if (!next) {
				tail.target = current;
				break;
			}
			path.push_back(current);
			steps.push_back(*next);
			current = next->new_binding;
		}
		if (path.empty()) {
			return resolutions.emplace(binding, std::move(tail)).first->second;
		}
		for (idx_t i = path.size(); i > 0; i--) {
			auto &step = steps[i - 1].get();
			if (!tail.replace_type && step.replace_type) {
				tail.replace_type = true;
				tail.new_type = step.new_type;
			}
			resolutions[path[i - 1]] = tail;
		}
		return resolutions.find(binding)->second;
	}

	const column_binding_set_t &new_bindings;
	const BindingReplacementGraph &replacements;
	column_binding_map_t<Resolution> resolutions;
};

} // namespace

static vector<ReplacementBinding> ScopeToOutput(OutputBindingResolver &resolver,
                                                const column_binding_set_t &new_bindings,
                                                const BindingReplacementGraph &replacements) {
	vector<ReplacementBinding> result;
	for (auto &replacement : replacements) {
		if (new_bindings.find(replacement.old_binding) != new_bindings.end()) {
			continue;
		}

		ReplacementBinding output_replacement(replacement.old_binding, replacement.old_binding);
		if (!resolver.TryResolve(replacement.old_binding, output_replacement)) {
			continue;
		}
		result.push_back(std::move(output_replacement));
	}
	return result;
}

void ColumnBindingRewrite::ValidateOutput(const vector<ColumnBinding> &old_output,
                                          const vector<ColumnBinding> &new_output,
                                          const BindingReplacementGraph &replacements) {
	column_binding_set_t new_bindings(new_output.begin(), new_output.end());
	OutputBindingResolver resolver(new_bindings, replacements);
	for (auto &binding : old_output) {
		ReplacementBinding resolved(binding, binding);
		if (!resolver.TryResolve(binding, resolved)) {
			throw InternalException("Binding rewrite lost output binding %s (resolved to %s, output %s)",
			                        binding.ToString(), resolved.new_binding.ToString(),
			                        LogicalOperator::ColumnBindingsToString(new_output));
		}
	}
}

void ColumnBindingRewrite::RewriteChild(
    unique_ptr<LogicalOperator> &op, idx_t child_index,
    const std::function<void(unique_ptr<LogicalOperator> &, BindingReplacementGraph &)> &rewrite) {
	if (child_index >= op->children.size()) {
		throw InternalException("Binding rewrite child index %llu out of range", child_index);
	}
	auto projection_map = LogicalOperatorVisitor::GetProjectionMap(*op, child_index);
	auto old_child_bindings =
	    projection_map ? op->children[child_index]->GetColumnBindings() : CaptureChildBindings(*op, child_index);
	if (projection_map && projection_map->empty()) {
		// Freeze the implicit selection before the callback can change the child's output layout.
		for (idx_t i = 0; i < old_child_bindings.size(); i++) {
			projection_map->emplace_back(i);
		}
	}
	BindingReplacementGraph replacements;
	rewrite(op->children[child_index], replacements);
	if (!op->children[child_index]) {
		throw InternalException("Child rewrite requires a non-null child");
	}
	if (projection_map && old_child_bindings.empty() && !op->children[child_index]->GetColumnBindings().empty()) {
		throw InternalException("Child rewrite cannot preserve an empty implicit projection");
	}
	ApplyToChild(op, child_index, std::move(old_child_bindings), replacements);
}

vector<ColumnBinding> ColumnBindingRewrite::CaptureChildBindings(LogicalOperator &op, idx_t child_index) {
#ifndef D_ASSERT_IS_ENABLED
	if (!op.HasProjectionMap()) {
		return {};
	}
#endif
	return op.children[child_index]->GetColumnBindings();
}

void ColumnBindingRewrite::ApplyToChild(unique_ptr<LogicalOperator> &op, idx_t child_index,
                                        vector<ColumnBinding> old_child_bindings,
                                        const BindingReplacementGraph &replacements) {
	if (child_index >= op->children.size()) {
		throw InternalException("Binding rewrite child index %llu out of range", child_index);
	}
	if (replacements.Empty() && !op->HasProjectionMap()) {
		return;
	}
	auto new_child_bindings = op->children[child_index]->GetColumnBindings();
	column_binding_set_t new_bindings(new_child_bindings.begin(), new_child_bindings.end());
	OutputBindingResolver resolver(new_bindings, replacements);
	auto boundary_replacements = ScopeToOutput(resolver, new_bindings, replacements);
	auto original_child_bindings = old_child_bindings;
	for (auto &binding : old_child_bindings) {
		if (new_bindings.find(binding) != new_bindings.end() || !replacements.Find(binding)) {
			continue;
		}
		ReplacementBinding resolved(binding, binding);
		if (!resolver.TryResolve(binding, resolved)) {
			throw InternalException("Binding rewrite moved child binding %s outside rewritten child output %s",
			                        binding.ToString(), LogicalOperator::ColumnBindingsToString(new_child_bindings));
		}
		binding = resolved.new_binding;
	}
	if (op->HasProjectionMap()) {
		auto projection_map = LogicalOperatorVisitor::GetProjectionMap(*op, child_index);
		D_ASSERT(projection_map);
		RemapProjectionMapStrict(*projection_map, original_child_bindings, old_child_bindings, new_child_bindings);
	}
	if (boundary_replacements.empty()) {
		return;
	}
	CorrelatedColumnBindingReplacer replacer;
	replacer.replacement_bindings = std::move(boundary_replacements);
	if (op->type == LogicalOperatorType::LOGICAL_DEPENDENT_JOIN && child_index == 0) {
		replacer.stop_operator = *op->children[child_index];
		replacer.VisitOperator(*op);
	} else {
		replacer.VisitOperatorBindings(*op);
	}
}

#ifdef D_ASSERT_IS_ENABLED
namespace {

class ReplacedBindingVerifier : public LogicalOperatorVisitor {
public:
	ReplacedBindingVerifier(const vector<ReplacementBinding> &replacements, LogicalOperator &replaced_subtree_p)
	    : replaced_subtree(replaced_subtree_p) {
		for (auto &replacement : replacements) {
			old_bindings.insert(replacement.old_binding);
		}
	}

	void VisitOperator(LogicalOperator &op) override {
		if (&op != &replaced_subtree) {
			LogicalOperatorVisitor::VisitOperator(op);
		}
	}

protected:
	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *expr_ptr) override {
		D_ASSERT(old_bindings.find(expr.Binding()) == old_bindings.end());
		return nullptr;
	}

private:
	LogicalOperator &replaced_subtree;
	column_binding_set_t old_bindings;
};

} // namespace
#endif

AncestorBindingReplacer::AncestorBindingReplacer(ColumnBindingReplacer &replacer_p) : replacer(replacer_p) {
	for (auto &replacement : replacer.replacement_bindings) {
		new_bindings.insert(replacement.new_binding);
	}
}

bool AncestorBindingReplacer::Replace(LogicalOperator &ancestor) {
	replacer.VisitOperatorBindings(ancestor);
	for (auto &binding : ancestor.GetColumnBindings()) {
		if (new_bindings.find(binding) != new_bindings.end()) {
			return true;
		}
	}
	return false;
}

void AncestorBindingReplacer::Verify([[maybe_unused]] LogicalOperator &root,
                                     [[maybe_unused]] LogicalOperator &replaced_subtree) const {
#ifdef D_ASSERT_IS_ENABLED
	DUCKDB_DEBUG_VERIFY_GUARD();
	ReplacedBindingVerifier verifier(replacer.replacement_bindings, replaced_subtree);
	verifier.VisitOperator(root);
#endif
}

} // namespace duckdb
