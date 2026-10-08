#include "duckdb/planner/filter/zonemap_checker.hpp"

#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/expression_barrier.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Checker instances
//===--------------------------------------------------------------------===//
template <class T>
static FilterPropagateResult CheckRange(ExpressionType comparison, T min_value, T max_value, T constant) {
	switch (comparison) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		if (Equals::Operation(constant, min_value) && Equals::Operation(constant, max_value)) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		if (!(LessThan::Operation(constant, min_value) || GreaterThan::Operation(constant, max_value))) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	case ExpressionType::COMPARE_NOTEQUAL:
	case ExpressionType::COMPARE_DISTINCT_FROM:
		if (LessThan::Operation(constant, min_value) || GreaterThan::Operation(constant, max_value)) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		if (Equals::Operation(constant, min_value) && Equals::Operation(constant, max_value)) {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		if (GreaterThanEquals::Operation(min_value, constant)) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		if (GreaterThanEquals::Operation(max_value, constant)) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	case ExpressionType::COMPARE_GREATERTHAN:
		if (GreaterThan::Operation(min_value, constant)) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		if (GreaterThan::Operation(max_value, constant)) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		if (LessThanEquals::Operation(max_value, constant)) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		if (LessThanEquals::Operation(min_value, constant)) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	case ExpressionType::COMPARE_LESSTHAN:
		if (LessThan::Operation(max_value, constant)) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		if (LessThan::Operation(min_value, constant)) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	default:
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
}

static FilterPropagateResult AllNullComparisonVerdict(ExpressionType comparison) {
	return comparison == ExpressionType::COMPARE_DISTINCT_FROM ? FilterPropagateResult::FILTER_ALWAYS_TRUE
	                                                           : FilterPropagateResult::FILTER_ALWAYS_FALSE;
}

static FilterPropagateResult ComparisonVerdict(const BaseStatistics &stats, ExpressionType comparison,
                                               FilterPropagateResult result) {
	if (stats.CanHaveNull() &&
	    (result != FilterPropagateResult::FILTER_ALWAYS_FALSE || comparison == ExpressionType::COMPARE_DISTINCT_FROM)) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	return result;
}

static FilterPropagateResult BetweenVerdict(const BaseStatistics &stats, FilterPropagateResult lower_result,
                                            FilterPropagateResult upper_result) {
	if (lower_result == FilterPropagateResult::FILTER_ALWAYS_FALSE ||
	    upper_result == FilterPropagateResult::FILTER_ALWAYS_FALSE) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	if (lower_result == FilterPropagateResult::FILTER_ALWAYS_TRUE &&
	    upper_result == FilterPropagateResult::FILTER_ALWAYS_TRUE && !stats.CanHaveNull()) {
		return FilterPropagateResult::FILTER_ALWAYS_TRUE;
	}
	return FilterPropagateResult::NO_PRUNING_POSSIBLE;
}

static FilterPropagateResult InVerdict(const BaseStatistics &stats, FilterPropagateResult result) {
	if (result == FilterPropagateResult::FILTER_ALWAYS_TRUE && stats.CanHaveNull()) {
		return FilterPropagateResult::FILTER_TRUE_OR_NULL;
	}
	return result;
}

static FilterPropagateResult NotVerdict(FilterPropagateResult child_result) {
	if (child_result == FilterPropagateResult::FILTER_ALWAYS_TRUE) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	if (child_result == FilterPropagateResult::FILTER_TRUE_OR_NULL) {
		return FilterPropagateResult::FILTER_FALSE_OR_NULL;
	}
	return FilterPropagateResult::NO_PRUNING_POSSIBLE;
}

template <class T, ExpressionType COMPARISON>
class ComparisonZonemapChecker final : public ZonemapChecker {
public:
	explicit ComparisonZonemapChecker(const Value &constant_p) : constant(constant_p.GetValueUnsafe<T>()) {
	}

	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		if (!stats.CanHaveNoNull()) {
			return AllNullComparisonVerdict(COMPARISON);
		}
		auto result = FilterPropagateResult::NO_PRUNING_POSSIBLE;
		if (NumericStats::HasMinMax(stats)) {
			result = CheckRange<T>(COMPARISON, NumericStats::GetMinUnsafe<T>(stats),
			                       NumericStats::GetMaxUnsafe<T>(stats), constant);
		}
		return ComparisonVerdict(stats, COMPARISON, result);
	}

private:
	T constant;
};

template <class T>
class BetweenZonemapChecker final : public ZonemapChecker {
public:
	BetweenZonemapChecker(ExpressionType lower_comparison_p, const Value &lower_p, ExpressionType upper_comparison_p,
	                      const Value &upper_p)
	    : lower_comparison(lower_comparison_p), upper_comparison(upper_comparison_p),
	      lower(lower_p.GetValueUnsafe<T>()), upper(upper_p.GetValueUnsafe<T>()) {
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
		return BetweenVerdict(stats, CheckRange<T>(lower_comparison, min_value, max_value, lower),
		                      CheckRange<T>(upper_comparison, min_value, max_value, upper));
	}

private:
	ExpressionType lower_comparison;
	ExpressionType upper_comparison;
	T lower;
	T upper;
};

template <class T>
static hugeint_t ToHugeint(T value) {
	return Hugeint::Convert(value);
}

static hugeint_t ToHugeint(hugeint_t value) {
	return value;
}

template <bool COVER_RANGE>
struct RangeCover {
	template <class T>
	static bool Covers(T min_value, T max_value, idx_t count) {
		return false;
	}
};

template <>
struct RangeCover<true> {
	template <class T>
	static bool Covers(T min_value, T max_value, idx_t count) {
		hugeint_t range;
		if (!TrySubtractOperator::Operation(ToHugeint(max_value), ToHugeint(min_value), range)) {
			return false;
		}
		return range == hugeint_t(NumericCast<int64_t>(count - 1));
	}
};

template <class T, bool COVER_RANGE>
class InZonemapChecker final : public ZonemapChecker {
public:
	explicit InZonemapChecker(const vector<const Value *> &values) {
		constants.reserve(values.size());
		for (auto *value : values) {
			constants.push_back(value->GetValueUnsafe<T>());
		}
		std::sort(constants.begin(), constants.end(), ConstantLess);
		constants.erase(std::unique(constants.begin(), constants.end(), ConstantEquals), constants.end());
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
		const auto lower = std::lower_bound(constants.begin(), constants.end(), min_value, ConstantLess);
		const auto upper = std::upper_bound(lower, constants.end(), max_value, ConstantLess);
		if (lower == upper) {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
		if (Equals::Operation(min_value, max_value) ||
		    RangeCover<COVER_RANGE>::Covers(min_value, max_value, NumericCast<idx_t>(upper - lower))) {
			return InVerdict(stats, FilterPropagateResult::FILTER_ALWAYS_TRUE);
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}

private:
	static bool ConstantLess(const T &left, const T &right) {
		return LessThan::Operation(left, right);
	}
	static bool ConstantEquals(const T &left, const T &right) {
		return Equals::Operation(left, right);
	}

private:
	vector<T> constants;
};

class StringComparisonZonemapChecker final : public ZonemapChecker {
public:
	StringComparisonZonemapChecker(ExpressionType comparison_p, string_t constant_p)
	    : comparison(comparison_p), constant(constant_p) {
	}

	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		if (!stats.CanHaveNoNull()) {
			return AllNullComparisonVerdict(comparison);
		}
		auto result = FilterPropagateResult::NO_PRUNING_POSSIBLE;
		if (stats.GetStatsType() == StatisticsType::STRING_STATS) {
			result = StringStats::CheckZonemap(stats, comparison, constant);
		}
		return ComparisonVerdict(stats, comparison, result);
	}

private:
	ExpressionType comparison;
	string_t constant;
};

class StringBetweenZonemapChecker final : public ZonemapChecker {
public:
	StringBetweenZonemapChecker(ExpressionType lower_comparison_p, string_t lower_p, ExpressionType upper_comparison_p,
	                            string_t upper_p)
	    : lower_comparison(lower_comparison_p), upper_comparison(upper_comparison_p), lower(lower_p), upper(upper_p) {
	}

	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		if (!stats.CanHaveNoNull()) {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
		if (stats.GetStatsType() != StatisticsType::STRING_STATS) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return BetweenVerdict(stats, StringStats::CheckZonemap(stats, lower_comparison, lower),
		                      StringStats::CheckZonemap(stats, upper_comparison, upper));
	}

private:
	ExpressionType lower_comparison;
	ExpressionType upper_comparison;
	string_t lower;
	string_t upper;
};

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
		if (stats.GetStatsType() != StatisticsType::STRING_STATS) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		for (const auto &constant : constants) {
			auto prune_result = StringStats::CheckZonemap(stats, ExpressionType::COMPARE_EQUAL, constant);
			if (prune_result != FilterPropagateResult::FILTER_ALWAYS_FALSE) {
				return InVerdict(stats, prune_result);
			}
		}
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}

private:
	vector<string_t> constants;
};

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

template <bool NEGATED>
class BoolZonemapChecker final : public ZonemapChecker {
public:
	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		if (stats.GetType().id() != LogicalTypeId::BOOLEAN) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		if (!stats.CanHaveNoNull()) {
			return FilterPropagateResult::FILTER_FALSE_OR_NULL;
		}
		if (!NumericStats::HasMinMax(stats)) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		const auto min_value = NumericStats::GetMinUnsafe<bool>(stats);
		if (min_value != NumericStats::GetMaxUnsafe<bool>(stats)) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		if (min_value == NEGATED) {
			return stats.CanHaveNull() ? FilterPropagateResult::FILTER_FALSE_OR_NULL
			                           : FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
		return stats.CanHaveNull() ? FilterPropagateResult::FILTER_TRUE_OR_NULL
		                           : FilterPropagateResult::FILTER_ALWAYS_TRUE;
	}
};

class NotZonemapChecker final : public ZonemapChecker {
public:
	explicit NotZonemapChecker(unique_ptr<ZonemapChecker> child_p) : child(std::move(child_p)) {
	}

	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		return NotVerdict(child->Check(stats, context));
	}

	bool IsFullyCompiled() const final {
		return child->IsFullyCompiled();
	}

private:
	unique_ptr<ZonemapChecker> child;
};

template <bool IS_AND>
static FilterPropagateResult CombineConjunction(FilterPropagateResult result, FilterPropagateResult child_result) {
	if (IS_AND) {
		if (child_result == FilterPropagateResult::FILTER_ALWAYS_FALSE ||
		    child_result == FilterPropagateResult::FILTER_FALSE_OR_NULL) {
			return child_result;
		}
		if (result == FilterPropagateResult::FILTER_ALWAYS_TRUE &&
		    child_result == FilterPropagateResult::FILTER_ALWAYS_TRUE) {
			return result;
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	switch (child_result) {
	case FilterPropagateResult::NO_PRUNING_POSSIBLE:
	case FilterPropagateResult::FILTER_ALWAYS_TRUE:
	case FilterPropagateResult::FILTER_TRUE_OR_NULL:
		return child_result;
	case FilterPropagateResult::FILTER_FALSE_OR_NULL:
		return result == FilterPropagateResult::FILTER_ALWAYS_FALSE ? child_result : result;
	default:
		return result;
	}
}

template <bool IS_AND>
static bool IsTerminal(FilterPropagateResult result) {
	if (IS_AND) {
		return result == FilterPropagateResult::FILTER_ALWAYS_FALSE ||
		       result == FilterPropagateResult::FILTER_FALSE_OR_NULL;
	}
	return result == FilterPropagateResult::NO_PRUNING_POSSIBLE || result == FilterPropagateResult::FILTER_ALWAYS_TRUE;
}

template <bool IS_AND>
static FilterPropagateResult ConjunctionIdentity() {
	return IS_AND ? FilterPropagateResult::FILTER_ALWAYS_TRUE : FilterPropagateResult::FILTER_ALWAYS_FALSE;
}

template <bool IS_AND>
class ConjunctionZonemapChecker final : public ZonemapChecker {
public:
	ConjunctionZonemapChecker(vector<unique_ptr<ZonemapChecker>> children_p, FilterPropagateResult folded_p)
	    : children(std::move(children_p)), folded(folded_p) {
		for (auto &child : children) {
			fully_compiled = fully_compiled && child->IsFullyCompiled();
		}
	}

	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		auto result = ConjunctionIdentity<IS_AND>();
		for (auto &child : children) {
			result = CombineConjunction<IS_AND>(result, child->Check(stats, context));
			if (IsTerminal<IS_AND>(result)) {
				return result;
			}
		}
		return CombineConjunction<IS_AND>(result, folded);
	}

	bool IsFullyCompiled() const final {
		return fully_compiled;
	}

private:
	vector<unique_ptr<ZonemapChecker>> children;
	FilterPropagateResult folded;
	bool fully_compiled = true;
};

class PruneCallbackZonemapChecker final : public ZonemapChecker {
public:
	PruneCallbackZonemapChecker(const BoundFunctionExpression &function_p,
	                            vector<unique_ptr<BaseStatistics>> constant_stats_p, vector<idx_t> column_children_p)
	    : function(function_p), constant_stats(std::move(constant_stats_p)),
	      column_children(std::move(column_children_p)) {
		child_stats.reserve(constant_stats.size());
		for (auto &stats : constant_stats) {
			child_stats.emplace_back(stats.get());
		}
	}

	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		for (auto child_idx : column_children) {
			child_stats[child_idx] = &stats;
		}
		FunctionStatisticsPruneInput input(function, function.BindInfo().get(), child_stats);
		return function.Function().GetFilterPruneCallback()(input);
	}

	bool IsFullyCompiled() const final {
		return false;
	}

private:
	const BoundFunctionExpression &function;
	vector<unique_ptr<BaseStatistics>> constant_stats;
	vector<idx_t> column_children;
	mutable vector<optional_ptr<const BaseStatistics>> child_stats;
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
	FallbackZonemapChecker(const Expression &expr_p, bool use_context_p) : expr(expr_p), use_context(use_context_p) {
	}

	FilterPropagateResult Check(const BaseStatistics &stats, optional_ptr<ClientContext> context) const final {
		return ExpressionFilter::CheckExpressionStatistics(use_context ? context : nullptr, expr, stats);
	}

	bool IsFullyCompiled() const final {
		return false;
	}

private:
	const Expression &expr;
	bool use_context;
};

//===--------------------------------------------------------------------===//
// Compile
//===--------------------------------------------------------------------===//
template <class OP, class... ARGS>
static unique_ptr<ZonemapChecker> MakeIntegralChecker(PhysicalType physical, ARGS &&... args) {
	switch (physical) {
	case PhysicalType::INT8:
		return OP::template Make<int8_t>(std::forward<ARGS>(args)...);
	case PhysicalType::INT16:
		return OP::template Make<int16_t>(std::forward<ARGS>(args)...);
	case PhysicalType::INT32:
		return OP::template Make<int32_t>(std::forward<ARGS>(args)...);
	case PhysicalType::INT64:
		return OP::template Make<int64_t>(std::forward<ARGS>(args)...);
	case PhysicalType::UINT8:
		return OP::template Make<uint8_t>(std::forward<ARGS>(args)...);
	case PhysicalType::UINT16:
		return OP::template Make<uint16_t>(std::forward<ARGS>(args)...);
	case PhysicalType::UINT32:
		return OP::template Make<uint32_t>(std::forward<ARGS>(args)...);
	case PhysicalType::UINT64:
		return OP::template Make<uint64_t>(std::forward<ARGS>(args)...);
	case PhysicalType::INT128:
		return OP::template Make<hugeint_t>(std::forward<ARGS>(args)...);
	default:
		return nullptr;
	}
}

template <class OP, class... ARGS>
static unique_ptr<ZonemapChecker> MakeNumericChecker(PhysicalType physical, ARGS &&... args) {
	switch (physical) {
	case PhysicalType::BOOL:
		return OP::template Make<bool>(std::forward<ARGS>(args)...);
	case PhysicalType::UINT128:
		return OP::template Make<uhugeint_t>(std::forward<ARGS>(args)...);
	case PhysicalType::FLOAT:
		return OP::template Make<float>(std::forward<ARGS>(args)...);
	case PhysicalType::DOUBLE:
		return OP::template Make<double>(std::forward<ARGS>(args)...);
	case PhysicalType::INTERVAL:
		return OP::template Make<interval_t>(std::forward<ARGS>(args)...);
	default:
		return MakeIntegralChecker<OP>(physical, std::forward<ARGS>(args)...);
	}
}

struct MakeComparisonChecker {
	template <class T>
	static unique_ptr<ZonemapChecker> Make(ExpressionType comparison, const Value &constant) {
		switch (comparison) {
		case ExpressionType::COMPARE_EQUAL:
		case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
			return make_uniq<ComparisonZonemapChecker<T, ExpressionType::COMPARE_EQUAL>>(constant);
		case ExpressionType::COMPARE_NOTEQUAL:
			return make_uniq<ComparisonZonemapChecker<T, ExpressionType::COMPARE_NOTEQUAL>>(constant);
		case ExpressionType::COMPARE_DISTINCT_FROM:
			return make_uniq<ComparisonZonemapChecker<T, ExpressionType::COMPARE_DISTINCT_FROM>>(constant);
		case ExpressionType::COMPARE_LESSTHAN:
			return make_uniq<ComparisonZonemapChecker<T, ExpressionType::COMPARE_LESSTHAN>>(constant);
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			return make_uniq<ComparisonZonemapChecker<T, ExpressionType::COMPARE_LESSTHANOREQUALTO>>(constant);
		case ExpressionType::COMPARE_GREATERTHAN:
			return make_uniq<ComparisonZonemapChecker<T, ExpressionType::COMPARE_GREATERTHAN>>(constant);
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			return make_uniq<ComparisonZonemapChecker<T, ExpressionType::COMPARE_GREATERTHANOREQUALTO>>(constant);
		default:
			return nullptr;
		}
	}
};

struct MakeBetweenChecker {
	template <class T>
	static unique_ptr<ZonemapChecker> Make(ExpressionType lower_comparison, const Value &lower,
	                                       ExpressionType upper_comparison, const Value &upper) {
		return make_uniq<BetweenZonemapChecker<T>>(lower_comparison, lower, upper_comparison, upper);
	}
};

template <bool COVER_RANGE>
struct MakeInChecker {
	template <class T>
	static unique_ptr<ZonemapChecker> Make(const vector<const Value *> &values) {
		return make_uniq<InZonemapChecker<T, COVER_RANGE>>(values);
	}
};

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

static CompileResult CompileChecker(const Expression &expr, bool use_context);

static bool IsColumnRef(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_REF &&
	       expr.Cast<BoundReferenceExpression>().Index() == 0;
}

static bool MayDeriveStats(const Expression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_REF:
		return IsColumnRef(expr);
	case ExpressionClass::BOUND_CONSTANT:
	case ExpressionClass::BOUND_FUNCTION:
	case ExpressionClass::BOUND_CASE:
		return true;
	case ExpressionClass::BOUND_OPERATOR:
		return expr.GetExpressionType() == ExpressionType::OPERATOR_COALESCE &&
		       !expr.Cast<BoundOperatorExpression>().GetChildren().empty();
	default:
		return false;
	}
}

static CompileResult CompileRemappedInput(const Expression &input) {
	if (MayDeriveStats(input)) {
		return {};
	}
	return FilterPropagateResult::NO_PRUNING_POSSIBLE;
}

static CompileResult CompileComparison(const BoundFunctionExpression &func) {
	auto comparison = func.GetExpressionType();
	auto &left = BoundComparisonExpression::Left(func);
	auto &right = BoundComparisonExpression::Right(func);
	optional_ptr<const Expression> input;
	optional_ptr<const BoundConstantExpression> constant_expr;
	if (right.GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
		input = &left;
		constant_expr = &right.Cast<BoundConstantExpression>();
	} else if (left.GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
		input = &right;
		constant_expr = &left.Cast<BoundConstantExpression>();
		comparison = FlipComparisonExpression(comparison);
	} else {
		if (MayDeriveStats(left) && MayDeriveStats(right)) {
			return {};
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	if (!IsColumnRef(*input)) {
		return CompileRemappedInput(*input);
	}
	auto &value = constant_expr->GetValue();
	if (value.IsNull()) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &input_type = input->GetReturnType();
	if (input_type.id() == LogicalTypeId::VARIANT || value.type().id() == LogicalTypeId::VARIANT) {
		return {};
	}
	if (value.type() != input_type) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	const auto physical = input_type.InternalType();
	auto checker = MakeNumericChecker<MakeComparisonChecker>(physical, comparison, value);
	if (checker) {
		return std::move(checker);
	}
	if (physical == PhysicalType::VARCHAR) {
		return make_uniq<StringComparisonZonemapChecker>(comparison, StringValue::Get(value));
	}
	return make_uniq<NullFlagsZonemapChecker>(AllNullComparisonVerdict(comparison));
}

static CompileResult CompileBetween(const BoundFunctionExpression &between) {
	auto &lower = BoundBetweenExpression::LowerBound(between);
	auto &upper = BoundBetweenExpression::UpperBound(between);
	if (lower.GetExpressionType() != ExpressionType::VALUE_CONSTANT ||
	    upper.GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &lower_value = lower.Cast<BoundConstantExpression>().GetValue();
	auto &upper_value = upper.Cast<BoundConstantExpression>().GetValue();
	if (lower_value.IsNull() || upper_value.IsNull()) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &input = BoundBetweenExpression::Input(between);
	if (!IsColumnRef(input)) {
		return CompileRemappedInput(input);
	}
	auto &input_type = input.GetReturnType();
	if (input_type.id() == LogicalTypeId::VARIANT || lower_value.type() != input_type ||
	    upper_value.type() != input_type) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	const auto lower_comparison = BoundBetweenExpression::LowerComparisonType(between);
	const auto upper_comparison = BoundBetweenExpression::UpperComparisonType(between);
	const auto physical = input_type.InternalType();
	auto checker =
	    MakeNumericChecker<MakeBetweenChecker>(physical, lower_comparison, lower_value, upper_comparison, upper_value);
	if (checker) {
		return std::move(checker);
	}
	if (physical == PhysicalType::VARCHAR) {
		return make_uniq<StringBetweenZonemapChecker>(lower_comparison, StringValue::Get(lower_value), upper_comparison,
		                                              StringValue::Get(upper_value));
	}
	return make_uniq<NullFlagsZonemapChecker>(FilterPropagateResult::FILTER_ALWAYS_FALSE);
}

static CompileResult CompileIn(const BoundOperatorExpression &op) {
	if (op.GetChildren().size() <= 1) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &input = *op.GetChildren()[0];
	if (!IsColumnRef(input)) {
		return CompileRemappedInput(input);
	}
	auto &input_type = input.GetReturnType();
	vector<const Value *> values;
	values.reserve(op.GetChildren().size() - 1);
	for (idx_t i = 1; i < op.GetChildren().size(); i++) {
		auto &child = *op.GetChildren()[i];
		if (child.GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		auto &value = child.Cast<BoundConstantExpression>().GetValue();
		if (value.type() != input_type) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		if (!value.IsNull()) {
			values.push_back(&value);
		}
	}
	if (values.empty()) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	const auto physical = input_type.InternalType();
	unique_ptr<ZonemapChecker> checker;
	if (input_type.IsIntegral() && physical != PhysicalType::UINT128) {
		checker = MakeIntegralChecker<MakeInChecker<true>>(physical, values);
	} else {
		checker = MakeNumericChecker<MakeInChecker<false>>(physical, values);
	}
	if (checker) {
		return std::move(checker);
	}
	if (physical == PhysicalType::VARCHAR) {
		return make_uniq<StringInZonemapChecker>(values);
	}
	return make_uniq<NullFlagsZonemapChecker>(FilterPropagateResult::FILTER_ALWAYS_FALSE);
}

static CompileResult CompileNot(const BoundOperatorExpression &op, bool use_context) {
	if (op.GetChildren().size() != 1) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &child = *op.GetChildren()[0];
	if (child.GetExpressionType() == ExpressionType::COMPARE_IN) {
		auto &in_children = child.Cast<BoundOperatorExpression>().GetChildren();
		if (in_children.size() == 2 && in_children[1]->GetExpressionType() == ExpressionType::VALUE_CONSTANT &&
		    in_children[1]->Cast<BoundConstantExpression>().GetValue().IsNull()) {
			return FilterPropagateResult::FILTER_FALSE_OR_NULL;
		}
	}
	if (child.GetExpressionClass() == ExpressionClass::BOUND_REF) {
		if (IsColumnRef(child) && child.GetReturnType().id() == LogicalTypeId::BOOLEAN) {
			return make_uniq<BoolZonemapChecker<true>>();
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto compiled = CompileChecker(child, use_context);
	if (compiled.verdict) {
		return NotVerdict(*compiled.verdict);
	}
	if (!compiled.checker) {
		return {};
	}
	return make_uniq<NotZonemapChecker>(std::move(compiled.checker));
}

static CompileResult CompileOperator(const BoundOperatorExpression &op, bool use_context) {
	switch (op.GetExpressionType()) {
	case ExpressionType::OPERATOR_IS_NULL:
	case ExpressionType::OPERATOR_IS_NOT_NULL: {
		if (op.GetChildren().empty()) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		auto &child = *op.GetChildren()[0];
		if (!IsColumnRef(child)) {
			return CompileRemappedInput(child);
		}
		if (op.GetExpressionType() == ExpressionType::OPERATOR_IS_NULL) {
			return make_uniq<NullZonemapChecker<true>>();
		}
		return make_uniq<NullZonemapChecker<false>>();
	}
	case ExpressionType::COMPARE_IN:
		return CompileIn(op);
	case ExpressionType::OPERATOR_NOT:
		return CompileNot(op, use_context);
	default:
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
}

static CompileResult CompileOptionalFilter(const BoundFunctionExpression &func,
                                           optional_ptr<const Expression> child_filter) {
	if (!child_filter || func.GetChildren().empty()) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &column = *func.GetChildren()[0];
	if (!IsColumnRef(column)) {
		return CompileRemappedInput(column);
	}
	return CompileChecker(*child_filter, false);
}

static CompileResult CompilePruneCallback(const BoundFunctionExpression &func) {
	auto callback = func.Function().GetFilterPruneCallback();
	auto bind_info = func.BindInfo().get();
	if (callback == OptionalFilterScalarFun::FilterPrune) {
		if (!bind_info) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return CompileOptionalFilter(func, bind_info->Cast<OptionalFilterFunctionData>().child_filter_expr.get());
	}
	if (callback == SelectivityOptionalFilterScalarFun::FilterPrune) {
		if (!bind_info) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return CompileOptionalFilter(func,
		                             bind_info->Cast<SelectivityOptionalFilterFunctionData>().child_filter_expr.get());
	}
	if (callback == DynamicFilterScalarFun::FilterPrune) {
		if (!bind_info) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		auto &data = bind_info->Cast<DynamicFilterFunctionData>();
		if (!data.filter_data || func.GetChildren().empty()) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		auto &column = *func.GetChildren()[0];
		if (!IsColumnRef(column)) {
			return CompileRemappedInput(column);
		}
		return make_uniq<DynamicZonemapChecker>(data.filter_data);
	}
	auto &children = func.GetChildren();
	vector<unique_ptr<BaseStatistics>> constant_stats;
	vector<idx_t> column_children;
	constant_stats.reserve(children.size());
	for (idx_t child_idx = 0; child_idx < children.size(); child_idx++) {
		auto &child = *children[child_idx];
		unique_ptr<BaseStatistics> child_stats;
		if (IsColumnRef(child)) {
			column_children.push_back(child_idx);
		} else if (child.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			child_stats = BaseStatistics::FromConstant(child.Cast<BoundConstantExpression>().GetValue()).ToUnique();
		} else if (MayDeriveStats(child)) {
			return {};
		}
		constant_stats.push_back(std::move(child_stats));
	}
	return make_uniq<PruneCallbackZonemapChecker>(func, std::move(constant_stats), std::move(column_children));
}

static CompileResult CompileFunction(const BoundFunctionExpression &func, bool use_context) {
	if (ExpressionBarrier::IsBarrier(func)) {
		return CompileChecker(*func.GetChildren()[0], use_context);
	}
	if (func.GetExpressionType() == ExpressionType::COMPARE_BETWEEN) {
		return CompileBetween(func);
	}
	if (BoundComparisonExpression::IsComparison(func.GetExpressionType())) {
		return CompileComparison(func);
	}
	if (!func.Function().HasFilterPruneCallback()) {
		if (func.GetReturnType().id() != LogicalTypeId::BOOLEAN) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return {};
	}
	return CompilePruneCallback(func);
}

template <bool IS_AND>
static bool CollectConjunctionChildren(const BoundConjunctionExpression &conj, bool use_context,
                                       vector<unique_ptr<ZonemapChecker>> &children, FilterPropagateResult &folded,
                                       optional<FilterPropagateResult> &stop_verdict) {
	for (auto &child : conj.GetChildren()) {
		if (stop_verdict) {
			return true;
		}
		if (child->GetExpressionType() == conj.GetExpressionType()) {
			if (!CollectConjunctionChildren<IS_AND>(child->Cast<BoundConjunctionExpression>(), use_context, children,
			                                        folded, stop_verdict)) {
				return false;
			}
			continue;
		}
		auto compiled = CompileChecker(*child, use_context);
		if (compiled.verdict) {
			auto verdict = *compiled.verdict;
			if (verdict == ConjunctionIdentity<IS_AND>()) {
				continue;
			}
			if (IS_AND && IsTerminal<IS_AND>(verdict)) {
				children.push_back(make_uniq<ConstantZonemapChecker>(verdict));
				stop_verdict = verdict;
				continue;
			}
			if (!IS_AND && verdict == FilterPropagateResult::FILTER_ALWAYS_TRUE) {
				return false;
			}
			if (!IS_AND && folded == FilterPropagateResult::NO_PRUNING_POSSIBLE) {
				continue;
			}
			folded = CombineConjunction<IS_AND>(folded, verdict);
			continue;
		}
		if (!compiled.checker) {
			compiled.checker = make_uniq<FallbackZonemapChecker>(*child, use_context);
		}
		children.push_back(std::move(compiled.checker));
	}
	return true;
}

template <bool IS_AND>
static CompileResult CompileConjunction(const BoundConjunctionExpression &conj, bool use_context) {
	vector<unique_ptr<ZonemapChecker>> children;
	children.reserve(conj.GetChildren().size());
	auto folded = ConjunctionIdentity<IS_AND>();
	optional<FilterPropagateResult> stop_verdict;
	if (!CollectConjunctionChildren<IS_AND>(conj, use_context, children, folded, stop_verdict)) {
		return FilterPropagateResult::FILTER_ALWAYS_TRUE;
	}
	if (children.empty()) {
		return folded;
	}
	if (children.size() == 1) {
		if (stop_verdict) {
			return *stop_verdict;
		}
		if (folded == ConjunctionIdentity<IS_AND>()) {
			return std::move(children[0]);
		}
	}
	return make_uniq<ConjunctionZonemapChecker<IS_AND>>(std::move(children), folded);
}

static CompileResult CompileChecker(const Expression &expr, bool use_context) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		switch (conj.GetExpressionType()) {
		case ExpressionType::CONJUNCTION_AND:
			return CompileConjunction<true>(conj, use_context);
		case ExpressionType::CONJUNCTION_OR:
			return CompileConjunction<false>(conj, use_context);
		default:
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
	}
	case ExpressionClass::BOUND_OPERATOR:
		return CompileOperator(expr.Cast<BoundOperatorExpression>(), use_context);
	case ExpressionClass::BOUND_FUNCTION:
		return CompileFunction(expr.Cast<BoundFunctionExpression>(), use_context);
	case ExpressionClass::BOUND_REF:
		if (IsColumnRef(expr) && expr.GetReturnType().id() == LogicalTypeId::BOOLEAN) {
			return make_uniq<BoolZonemapChecker<false>>();
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	default:
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
}

unique_ptr<ZonemapChecker> ZonemapChecker::Compile(const Expression &expr) {
	auto compiled = CompileChecker(expr, true);
	if (compiled.checker) {
		return std::move(compiled.checker);
	}
	if (compiled.verdict) {
		return make_uniq<ConstantZonemapChecker>(*compiled.verdict);
	}
	return make_uniq<FallbackZonemapChecker>(expr, true);
}

} // namespace duckdb
