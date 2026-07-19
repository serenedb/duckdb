#include "duckdb/planner/filter/zonemap_checker.hpp"

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/dynamic_filter.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/geometry_stats.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Checker instances
//===--------------------------------------------------------------------===//
//! Comparison against a raw constant. OP tests the blanket-accept side first and the
//! can-any-pass side second; MIN_FIRST selects which bound is which (mirrors the per-operator
//! branches of NumericStats' zonemap kernel). Null-flag semantics of CheckComparisonStatistics.
template <class T, class OP, bool MIN_FIRST>
class ComparisonZonemapChecker final : public ZonemapChecker {
public:
	explicit ComparisonZonemapChecker(const Value &constant_p) : constant(constant_p.GetValueUnsafe<T>()) {
	}

	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		if (!stats.CanHaveNoNull()) {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
		if (stats.CanHaveNull() || !NumericStats::HasMinMax(stats)) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		const auto min_value = NumericStats::GetMinUnsafe<T>(stats);
		const auto max_value = NumericStats::GetMaxUnsafe<T>(stats);
		if (OP::Operation(MIN_FIRST ? min_value : max_value, constant)) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		if (OP::Operation(MIN_FIRST ? max_value : min_value, constant)) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}

private:
	T constant;
};

//! Equality / inequality against a raw constant (needs both bounds at once). ALL_NULL_TRUE is
//! the IS DISTINCT FROM special case: an all-null column IS distinct from any constant.
template <class T, bool NEGATED, bool ALL_NULL_TRUE = false>
class EqualityZonemapChecker final : public ZonemapChecker {
public:
	explicit EqualityZonemapChecker(const Value &constant_p) : constant(constant_p.GetValueUnsafe<T>()) {
	}

	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		if (!stats.CanHaveNoNull()) {
			return ALL_NULL_TRUE ? FilterPropagateResult::FILTER_ALWAYS_TRUE
			                     : FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
		if (stats.CanHaveNull() || !NumericStats::HasMinMax(stats)) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		const auto min_value = NumericStats::GetMinUnsafe<T>(stats);
		const auto max_value = NumericStats::GetMaxUnsafe<T>(stats);
		const bool in_range =
		    !(LessThan::Operation(constant, min_value) || GreaterThan::Operation(constant, max_value));
		const bool exact_range = Equals::Operation(constant, min_value) && Equals::Operation(constant, max_value);
		if (!in_range) {
			return NEGATED ? FilterPropagateResult::FILTER_ALWAYS_TRUE : FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
		if (exact_range) {
			return NEGATED ? FilterPropagateResult::FILTER_ALWAYS_FALSE : FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}

private:
	T constant;
};

//! IN over raw constants: null-flag semantics of CheckInOperatorStatistics, per-constant
//! equality verdicts combined like the multi-constant zonemap kernel.
template <class T>
class InZonemapChecker final : public ZonemapChecker {
public:
	explicit InZonemapChecker(const vector<const Value *> &values) {
		constants.reserve(values.size());
		for (auto *value : values) {
			constants.push_back(value->GetValueUnsafe<T>());
		}
	}

	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		if (!stats.CanHaveNoNull()) {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
		if (!NumericStats::HasMinMax(stats)) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		const auto min_value = NumericStats::GetMinUnsafe<T>(stats);
		const auto max_value = NumericStats::GetMaxUnsafe<T>(stats);
		auto result = FilterPropagateResult::FILTER_ALWAYS_FALSE;
		for (const auto &constant : constants) {
			if (LessThan::Operation(constant, min_value) || GreaterThan::Operation(constant, max_value)) {
				continue;
			}
			if (Equals::Operation(constant, min_value) && Equals::Operation(constant, max_value)) {
				result = FilterPropagateResult::FILTER_ALWAYS_TRUE;
				break;
			}
			result = FilterPropagateResult::NO_PRUNING_POSSIBLE;
			break;
		}
		if (result == FilterPropagateResult::FILTER_ALWAYS_TRUE && stats.CanHaveNull()) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return result;
	}

private:
	vector<T> constants;
};

//! Comparison against a string constant: prefix checks through the string statistics. The
//! constant is a view into the filter expression tree, which the checker never outlives
//! (FallbackZonemapChecker holds the same reference). Null-flag semantics of
//! CheckComparisonStatistics; `all_null_true` is the IS DISTINCT FROM special case.
class StringComparisonZonemapChecker final : public ZonemapChecker {
public:
	StringComparisonZonemapChecker(ExpressionType comparison_p, string_t constant_p, bool all_null_true_p)
	    : comparison(comparison_p), constant(constant_p), all_null_true(all_null_true_p) {
	}

	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		if (!stats.CanHaveNoNull()) {
			return all_null_true ? FilterPropagateResult::FILTER_ALWAYS_TRUE
			                     : FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
		if (stats.CanHaveNull() || stats.GetStatsType() != StatisticsType::STRING_STATS) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return StringStats::CheckZonemap(stats, comparison, constant);
	}

private:
	ExpressionType comparison;
	string_t constant;
	bool all_null_true;
};

//! IN over string constants (views into the filter expression tree): per-constant equality
//! verdicts combined like the multi-constant prefix kernel, null-flag semantics of
//! CheckInOperatorStatistics.
class StringInZonemapChecker final : public ZonemapChecker {
public:
	explicit StringInZonemapChecker(const vector<const Value *> &values) {
		constants.reserve(values.size());
		for (auto *value : values) {
			constants.emplace_back(StringValue::Get(*value));
		}
	}

	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		if (!stats.CanHaveNoNull()) {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
		auto result = FilterPropagateResult::NO_PRUNING_POSSIBLE;
		if (stats.GetStatsType() == StatisticsType::STRING_STATS) {
			result = FilterPropagateResult::FILTER_ALWAYS_FALSE;
			for (const auto &constant : constants) {
				auto prune_result = StringStats::CheckZonemap(stats, ExpressionType::COMPARE_EQUAL, constant);
				if (prune_result != FilterPropagateResult::FILTER_ALWAYS_FALSE) {
					result = prune_result;
					break;
				}
			}
		}
		if (result == FilterPropagateResult::FILTER_ALWAYS_TRUE && stats.CanHaveNull()) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return result;
	}

private:
	vector<string_t> constants;
};

//! Bounding-box check for the geometry intersection predicates (&&, st_intersects_extent)
//! over a plain column reference: the constant's extent is extracted once at compile.
class GeometryZonemapChecker final : public ZonemapChecker {
public:
	explicit GeometryZonemapChecker(const GeometryExtent &extent_p) : extent(extent_p) {
	}

	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		if (stats.GetStatsType() != StatisticsType::GEOMETRY_STATS) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		if (!stats.CanHaveNoNull()) {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
		return GeometryStats::CheckZonemap(stats, extent);
	}

private:
	GeometryExtent extent;
};

//! Physicals the walk can never zonemap-prune (interval, bool, bit, ...): only the all-null
//! verdict remains, exactly like CheckZonemapAgainstConstants' default.
class NullFlagsZonemapChecker final : public ZonemapChecker {
public:
	explicit NullFlagsZonemapChecker(FilterPropagateResult all_null_verdict_p) : all_null_verdict(all_null_verdict_p) {
	}

	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		if (!stats.CanHaveNoNull()) {
			return all_null_verdict;
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}

private:
	FilterPropagateResult all_null_verdict;
};

template <bool IS_NULL>
class NullZonemapChecker final : public ZonemapChecker {
public:
	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		if (IS_NULL ? !stats.CanHaveNull() : !stats.CanHaveNoNull()) {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
		if (IS_NULL ? !stats.CanHaveNoNull() : !stats.CanHaveNull()) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
};

template <bool IS_AND>
class ConjunctionZonemapChecker final : public ZonemapChecker {
public:
	explicit ConjunctionZonemapChecker(vector<unique_ptr<ZonemapChecker>> children_p)
	    : children(std::move(children_p)) {
		for (auto &child : children) {
			fully_compiled = fully_compiled && child->IsFullyCompiled();
		}
	}

	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		if (IS_AND) {
			auto result = FilterPropagateResult::FILTER_ALWAYS_TRUE;
			for (auto &child : children) {
				auto prune_result = child->Check(stats, context);
				if (prune_result == FilterPropagateResult::FILTER_ALWAYS_FALSE) {
					return FilterPropagateResult::FILTER_ALWAYS_FALSE;
				}
				if (prune_result != result) {
					result = FilterPropagateResult::NO_PRUNING_POSSIBLE;
				}
			}
			return result;
		}
		for (auto &child : children) {
			auto prune_result = child->Check(stats, context);
			if (prune_result == FilterPropagateResult::NO_PRUNING_POSSIBLE) {
				return FilterPropagateResult::NO_PRUNING_POSSIBLE;
			}
			if (prune_result == FilterPropagateResult::FILTER_ALWAYS_TRUE) {
				return FilterPropagateResult::FILTER_ALWAYS_TRUE;
			}
		}
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}

	bool IsFullyCompiled() const final {
		return fully_compiled;
	}

private:
	vector<unique_ptr<ZonemapChecker>> children;
	bool fully_compiled = true;
};

//! A prune-callback function over the plain column reference (bloom filters, prefix ranges):
//! the callback is self-contained over (bind data, stats), so the walk to reach it compiles
//! away (mirrors CheckFunctionStatistics). Callbacks probe real structures, so per-group hot
//! paths treat this as not fully compiled.
class PruneCallbackZonemapChecker final : public ZonemapChecker {
public:
	PruneCallbackZonemapChecker(propagate_filter_t callback_p, optional_ptr<FunctionData> bind_info_p)
	    : callback(callback_p), bind_info(bind_info_p) {
	}

	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		FunctionStatisticsPruneInput input(bind_info, stats);
		return callback(input);
	}

	bool IsFullyCompiled() const final {
		return false;
	}

private:
	propagate_filter_t callback;
	optional_ptr<FunctionData> bind_info;
};

//! Root dynamic filter: reads the shared bound the TOP_N operator tightens while the scan runs
//! (mirrors DynamicFilterScalarFun::FilterPrune).
class DynamicZonemapChecker final : public ZonemapChecker {
public:
	explicit DynamicZonemapChecker(shared_ptr<DynamicFilterData> filter_data_p)
	    : filter_data(std::move(filter_data_p)) {
	}

	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		lock_guard<mutex> lock(filter_data->lock);
		if (!filter_data->initialized) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return DynamicFilterData::CheckStatistics(stats, filter_data->comparison_type, filter_data->constant);
	}

private:
	shared_ptr<DynamicFilterData> filter_data;
};

//! A subtree whose verdict is known at compile time: materialized only when the whole filter
//! folds to a constant, or as the single retained stand-in for a conjunction's folded
//! NO_PRUNING children.
class ConstantZonemapChecker final : public ZonemapChecker {
public:
	explicit ConstantZonemapChecker(FilterPropagateResult result_p) : result(result_p) {
	}

	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		return result;
	}

private:
	FilterPropagateResult result;
};

//! Everything else: delegate the subtree to the expression walk.
class FallbackZonemapChecker final : public ZonemapChecker {
public:
	explicit FallbackZonemapChecker(const Expression &expr_p) : expr(expr_p) {
	}

	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		return ExpressionFilter::CheckExpressionStatistics(context, expr, stats);
	}

	bool IsFullyCompiled() const final {
		return false;
	}

private:
	const Expression &expr;
};

//===--------------------------------------------------------------------===//
// Compile
//===--------------------------------------------------------------------===//
template <template <class> class CHECKER, class... ARGS>
static unique_ptr<ZonemapChecker> MakeTypedChecker(PhysicalType physical, ARGS &&... args) {
	switch (physical) {
	case PhysicalType::INT8:
		return make_uniq<CHECKER<int8_t>>(std::forward<ARGS>(args)...);
	case PhysicalType::INT16:
		return make_uniq<CHECKER<int16_t>>(std::forward<ARGS>(args)...);
	case PhysicalType::INT32:
		return make_uniq<CHECKER<int32_t>>(std::forward<ARGS>(args)...);
	case PhysicalType::INT64:
		return make_uniq<CHECKER<int64_t>>(std::forward<ARGS>(args)...);
	case PhysicalType::UINT8:
		return make_uniq<CHECKER<uint8_t>>(std::forward<ARGS>(args)...);
	case PhysicalType::UINT16:
		return make_uniq<CHECKER<uint16_t>>(std::forward<ARGS>(args)...);
	case PhysicalType::UINT32:
		return make_uniq<CHECKER<uint32_t>>(std::forward<ARGS>(args)...);
	case PhysicalType::UINT64:
		return make_uniq<CHECKER<uint64_t>>(std::forward<ARGS>(args)...);
	case PhysicalType::INT128:
		return make_uniq<CHECKER<hugeint_t>>(std::forward<ARGS>(args)...);
	case PhysicalType::UINT128:
		return make_uniq<CHECKER<uhugeint_t>>(std::forward<ARGS>(args)...);
	case PhysicalType::FLOAT:
		return make_uniq<CHECKER<float>>(std::forward<ARGS>(args)...);
	case PhysicalType::DOUBLE:
		return make_uniq<CHECKER<double>>(std::forward<ARGS>(args)...);
	default:
		return nullptr;
	}
}

template <class T>
using EqualChecker = EqualityZonemapChecker<T, false>;
template <class T>
using NotEqualChecker = EqualityZonemapChecker<T, true>;
template <class T>
using IsDistinctChecker = EqualityZonemapChecker<T, true, true>;
template <class T>
using GreaterThanChecker = ComparisonZonemapChecker<T, GreaterThan, true>;
template <class T>
using GreaterThanEqualsChecker = ComparisonZonemapChecker<T, GreaterThanEquals, true>;
template <class T>
using LessThanChecker = ComparisonZonemapChecker<T, LessThan, false>;
template <class T>
using LessThanEqualsChecker = ComparisonZonemapChecker<T, LessThanEquals, false>;

static bool HasTypedChecker(PhysicalType physical) {
	switch (physical) {
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::INT128:
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
	case PhysicalType::UINT128:
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
		return true;
	default:
		return false;
	}
}

//! TryGetFilterStats remaps statistics through casts, constants and function statistics
//! callbacks; every other expression class makes the walk answer NO_PRUNING unconditionally.
static bool NeedsStatsRemap(const Expression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CAST:
	case ExpressionClass::BOUND_CONSTANT:
	case ExpressionClass::BOUND_FUNCTION:
		return true;
	default:
		return false;
	}
}

//! Compilation outcome of a subtree: a checker, a compile-time-constant verdict (folded by the
//! enclosing conjunction, materialized only at the top), or neither -- the subtree needs the
//! statistics remaps of the walk (materialized as FallbackZonemapChecker by the enclosure).
struct CompileResult {
	CompileResult() = default;
	template <class T>
	CompileResult(unique_ptr<T> checker_p) : checker(std::move(checker_p)) { // NOLINT
	}
	CompileResult(FilterPropagateResult verdict_p) : verdict(verdict_p) { // NOLINT
	}

	unique_ptr<ZonemapChecker> checker;
	optional<FilterPropagateResult> verdict;
};

static CompileResult CompileChecker(const Expression &expr);

static CompileResult CompileComparison(const BoundFunctionExpression &func) {
	auto *ref = &BoundComparisonExpression::Left(func);
	auto *cst = &BoundComparisonExpression::Right(func);
	auto comparison = func.GetExpressionType();
	if (cst->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		std::swap(ref, cst);
		comparison = FlipComparisonExpression(comparison);
	}
	if (cst->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		// neither side is a constant
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	if (ref->GetReturnType().id() == LogicalTypeId::VARIANT) {
		// shredded-variant pruning depends on the statistics instance
		return {};
	}
	if (ref->GetExpressionClass() != ExpressionClass::BOUND_REF) {
		// a cast / struct-extract / constant side needs the statistics remaps of the walk;
		// every other class makes the walk answer NO_PRUNING unconditionally
		if (NeedsStatsRemap(*ref)) {
			return {};
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &value = cst->Cast<BoundConstantExpression>().GetValue();
	if (value.IsNull()) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	if (value.type() != ref->GetReturnType()) {
		// unreachable in a bound tree; also covers VARIANT constants against non-variant
		// references, which the walk answers with NO_PRUNING
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	const auto physical = value.type().InternalType();
	if (!HasTypedChecker(physical)) {
		// strings check through their statistics prefixes; every other physical is the walk's
		// CheckZonemapAgainstConstants default (no pruning beyond the null flags)
		switch (comparison) {
		case ExpressionType::COMPARE_EQUAL:
		case ExpressionType::COMPARE_NOTEQUAL:
		case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		case ExpressionType::COMPARE_DISTINCT_FROM:
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			break;
		default:
			// IsComparison admits no other types
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		const bool all_null_true = comparison == ExpressionType::COMPARE_DISTINCT_FROM;
		if (physical == PhysicalType::VARCHAR) {
			return make_uniq<StringComparisonZonemapChecker>(comparison, StringValue::Get(value), all_null_true);
		}
		return make_uniq<NullFlagsZonemapChecker>(all_null_true ? FilterPropagateResult::FILTER_ALWAYS_TRUE
		                                                        : FilterPropagateResult::FILTER_ALWAYS_FALSE);
	}
	switch (comparison) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		return MakeTypedChecker<EqualChecker>(physical, value);
	case ExpressionType::COMPARE_NOTEQUAL:
		return MakeTypedChecker<NotEqualChecker>(physical, value);
	case ExpressionType::COMPARE_DISTINCT_FROM:
		// an all-null column IS distinct from any constant
		return MakeTypedChecker<IsDistinctChecker>(physical, value);
	case ExpressionType::COMPARE_LESSTHAN:
		return MakeTypedChecker<LessThanChecker>(physical, value);
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return MakeTypedChecker<LessThanEqualsChecker>(physical, value);
	case ExpressionType::COMPARE_GREATERTHAN:
		return MakeTypedChecker<GreaterThanChecker>(physical, value);
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return MakeTypedChecker<GreaterThanEqualsChecker>(physical, value);
	default:
		// IsComparison admits no other types
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
}

static CompileResult CompileIn(const BoundOperatorExpression &op) {
	// mirrors CheckInOperatorStatistics over the plain column reference
	if (op.GetChildren().size() <= 1) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &ref = *op.GetChildren()[0];
	if (ref.GetExpressionClass() != ExpressionClass::BOUND_REF) {
		if (NeedsStatsRemap(ref)) {
			return {};
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &ref_type = ref.GetReturnType();
	const auto physical = ref_type.InternalType();
	vector<const Value *> values;
	values.reserve(op.GetChildren().size() - 1);
	for (idx_t i = 1; i < op.GetChildren().size(); i++) {
		auto &child = *op.GetChildren()[i];
		if (child.GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
			// the walk gives up on non-constant IN elements
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		auto &value = child.Cast<BoundConstantExpression>().GetValue();
		if (value.type() != ref_type) {
			// unreachable in a bound tree; the walk would compare mismatched statistics
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		if (!value.IsNull()) {
			values.push_back(&value);
		}
	}
	if (values.empty()) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	if (!HasTypedChecker(physical)) {
		if (physical != PhysicalType::VARCHAR) {
			return make_uniq<NullFlagsZonemapChecker>(FilterPropagateResult::FILTER_ALWAYS_FALSE);
		}
		return make_uniq<StringInZonemapChecker>(values);
	}
	return MakeTypedChecker<InZonemapChecker>(physical, values);
}

//! The geometry intersection predicates (&&, st_intersects_extent) over a plain column
//! reference against a constant: mirrors GeometryStats::CheckZonemap with the constant's
//! extent extracted once. Everything else compiles to the walk's unconditional NO_PRUNING.
static CompileResult CompileGeometryPredicate(const BoundFunctionExpression &func) {
	if (func.GetReturnType() != LogicalType::BOOLEAN || func.GetChildren().size() != 2) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	if (func.GetChildren()[0]->GetReturnType().id() != LogicalTypeId::GEOMETRY ||
	    func.GetChildren()[1]->GetReturnType().id() != LogicalTypeId::GEOMETRY) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	static const case_insensitive_set_view_t geometry_predicates {"&&", "st_intersects_extent"};
	if (!geometry_predicates.contains(func.Function().GetName().GetIdentifierName())) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	// CRS-erasing GEOMETRY -> GEOMETRY casts only change metadata, not coordinates
	auto strip_geometry_cast = [](const Expression &child) -> const Expression * {
		if (child.GetExpressionType() == ExpressionType::OPERATOR_CAST) {
			auto &cast = child.Cast<BoundCastExpression>();
			if (cast.Child().GetReturnType().id() == LogicalTypeId::GEOMETRY) {
				return &cast.Child();
			}
		}
		return &child;
	};
	const auto &lhs = *strip_geometry_cast(*func.GetChildren()[0]);
	const auto &rhs = *strip_geometry_cast(*func.GetChildren()[1]);
	const auto lhs_kind = lhs.GetExpressionType();
	const auto rhs_kind = rhs.GetExpressionType();
	const auto lhs_is_const = lhs_kind == ExpressionType::VALUE_CONSTANT && rhs_kind == ExpressionType::BOUND_REF;
	const auto rhs_is_const = rhs_kind == ExpressionType::VALUE_CONSTANT && lhs_kind == ExpressionType::BOUND_REF;
	const Expression *constant_expr = lhs_is_const ? &lhs : (rhs_is_const ? &rhs : nullptr);
	if (!constant_expr) {
		// no constant argument: only the all-null verdict remains
		return make_uniq<NullFlagsZonemapChecker>(FilterPropagateResult::FILTER_ALWAYS_FALSE);
	}
	auto &constant = constant_expr->Cast<BoundConstantExpression>().GetValue();
	if (constant.IsNull() || constant.type().id() != LogicalTypeId::GEOMETRY) {
		return make_uniq<NullFlagsZonemapChecker>(FilterPropagateResult::FILTER_ALWAYS_FALSE);
	}
	auto extent = GeometryExtent::Empty();
	if (Geometry::GetExtent(string_t(StringValue::Get(constant)), extent) == 0) {
		// an empty geometry never matches
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	return make_uniq<GeometryZonemapChecker>(extent);
}

//! Collects a conjunction's compiled children, flattening same-type nesting from the expression
//! tree and folding compile-time constants: the absorbing verdict (ALWAYS_FALSE for AND,
//! ALWAYS_TRUE for OR) folds the whole conjunction (returns false), the identity verdict drops
//! out, and anything else only forbids the blanket verdict (reported via no_pruning_child).
static bool CollectConjunctionChildren(const BoundConjunctionExpression &conj, bool is_and,
                                       vector<unique_ptr<ZonemapChecker>> &children, bool &no_pruning_child) {
	const auto absorbing =
	    is_and ? FilterPropagateResult::FILTER_ALWAYS_FALSE : FilterPropagateResult::FILTER_ALWAYS_TRUE;
	const auto identity =
	    is_and ? FilterPropagateResult::FILTER_ALWAYS_TRUE : FilterPropagateResult::FILTER_ALWAYS_FALSE;
	for (auto &child : conj.GetChildren()) {
		if (child->GetExpressionType() == conj.GetExpressionType()) {
			if (!CollectConjunctionChildren(child->Cast<BoundConjunctionExpression>(), is_and, children,
			                                no_pruning_child)) {
				return false;
			}
			continue;
		}
		auto compiled = CompileChecker(*child);
		if (compiled.verdict) {
			if (*compiled.verdict == absorbing) {
				return false;
			}
			if (*compiled.verdict != identity) {
				no_pruning_child = true;
			}
			continue;
		}
		if (!compiled.checker) {
			// delegate just this subtree to the walk
			compiled.checker = make_uniq<FallbackZonemapChecker>(*child);
		}
		children.push_back(std::move(compiled.checker));
	}
	return true;
}

static CompileResult CompileChecker(const Expression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		const auto etype = conj.GetExpressionType();
		if (etype != ExpressionType::CONJUNCTION_AND && etype != ExpressionType::CONJUNCTION_OR) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		const bool is_and = etype == ExpressionType::CONJUNCTION_AND;
		vector<unique_ptr<ZonemapChecker>> children;
		children.reserve(conj.GetChildren().size());
		bool no_pruning_child = false;
		if (!CollectConjunctionChildren(conj, is_and, children, no_pruning_child)) {
			return is_and ? FilterPropagateResult::FILTER_ALWAYS_FALSE : FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		if (children.empty()) {
			if (no_pruning_child) {
				return FilterPropagateResult::NO_PRUNING_POSSIBLE;
			}
			return is_and ? FilterPropagateResult::FILTER_ALWAYS_TRUE : FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
		if (no_pruning_child) {
			// one retained stand-in carries every folded unknown child; kept last, so a
			// disjunction still short-circuits on a blanket accept first
			children.push_back(make_uniq<ConstantZonemapChecker>(FilterPropagateResult::NO_PRUNING_POSSIBLE));
		} else if (children.size() == 1) {
			return std::move(children[0]);
		}
		if (is_and) {
			return make_uniq<ConjunctionZonemapChecker<true>>(std::move(children));
		}
		return make_uniq<ConjunctionZonemapChecker<false>>(std::move(children));
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op = expr.Cast<BoundOperatorExpression>();
		const auto etype = op.GetExpressionType();
		if (etype == ExpressionType::OPERATOR_IS_NULL || etype == ExpressionType::OPERATOR_IS_NOT_NULL) {
			// CheckNullOperatorStatistics answers from the top-level statistics' null flags when
			// the child is the plain column reference; casts and extracts need the walk's remaps
			if (op.GetChildren().empty()) {
				return FilterPropagateResult::NO_PRUNING_POSSIBLE;
			}
			auto &child = *op.GetChildren()[0];
			if (child.GetExpressionClass() != ExpressionClass::BOUND_REF) {
				if (NeedsStatsRemap(child)) {
					return {};
				}
				return FilterPropagateResult::NO_PRUNING_POSSIBLE;
			}
			if (etype == ExpressionType::OPERATOR_IS_NULL) {
				return make_uniq<NullZonemapChecker<true>>();
			}
			return make_uniq<NullZonemapChecker<false>>();
		}
		if (etype == ExpressionType::COMPARE_IN) {
			return CompileIn(op);
		}
		// CheckOperatorStatistics has no other pruning operators
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (BoundComparisonExpression::IsComparison(expr)) {
			// mirrors CheckComparisonStatistics for a plain column reference against a constant
			return CompileComparison(func);
		}
		auto &name = func.Function().GetName();
		// the optional wrappers' FilterPrune delegates to the child filter expression verbatim
		if (name == OptionalFilterScalarFun::NAME && func.BindInfo()) {
			auto &data = func.BindInfo()->Cast<OptionalFilterFunctionData>();
			if (data.child_filter_expr) {
				return CompileChecker(*data.child_filter_expr);
			}
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		if (name == SelectivityOptionalFilterScalarFun::NAME && func.BindInfo()) {
			auto &data = func.BindInfo()->Cast<SelectivityOptionalFilterFunctionData>();
			if (data.child_filter_expr) {
				return CompileChecker(*data.child_filter_expr);
			}
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		if (name == DynamicFilterScalarFun::NAME && func.BindInfo()) {
			// the shared bound is read per check; mirrors DynamicFilterScalarFun::FilterPrune
			auto &data = func.BindInfo()->Cast<DynamicFilterFunctionData>();
			if (data.filter_data) {
				return make_uniq<DynamicZonemapChecker>(data.filter_data);
			}
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		if (!func.Function().HasFilterPruneCallback()) {
			// CheckFunctionStatistics answers before any statistics remap -- except the
			// geometry bounding-box predicates, which it recognizes structurally
			return CompileGeometryPredicate(func);
		}
		// generic prune callback (bloom filter, prefix range, future functions): compiles when
		// the statistics feed it unmapped -- no children, or the plain column reference first
		if (func.GetChildren().empty() || func.GetChildren()[0]->GetExpressionClass() == ExpressionClass::BOUND_REF) {
			return make_uniq<PruneCallbackZonemapChecker>(func.Function().GetFilterPruneCallback(), func.BindInfo());
		}
		if (NeedsStatsRemap(*func.GetChildren()[0])) {
			return {};
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	default:
		// CheckExpressionStatistics has no pruning for any other expression class
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
}

unique_ptr<ZonemapChecker> ZonemapChecker::Compile(const Expression &expr) {
	auto compiled = CompileChecker(expr);
	if (compiled.checker) {
		return std::move(compiled.checker);
	}
	if (compiled.verdict) {
		return make_uniq<ConstantZonemapChecker>(*compiled.verdict);
	}
	return make_uniq<FallbackZonemapChecker>(expr);
}

} // namespace duckdb
