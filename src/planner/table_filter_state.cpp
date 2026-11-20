#include "duckdb/planner/table_filter_state.hpp"
#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/type_util.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/filter/zonemap_checker.hpp"

#include <bit>

namespace duckdb {

static unique_ptr<ExpressionFilterExecutor> TryCreateFastExecutor(const Expression &expression,
                                                                  bool inside_selectivity_optional);

static void InitializeExecutor(ClientContext &context, const Expression &expression, ExpressionFilterState &state) {
	state.executor = make_uniq<ExpressionExecutor>(context);
	state.executor->AddExpression(expression);
}

ExpressionFilterState::ExpressionFilterState(ClientContext &context, const Expression &expression) {
	can_throw = expression.CanThrow();
	fast_executor = TryCreateFastExecutor(expression, false);
	zonemap_checker = ZonemapChecker::Compile(expression);
	InitializeExecutor(context, expression, *this);
}

ExpressionFilterState::~ExpressionFilterState() {
}

bool ExpressionFilterExecutor::FilterValue(const_data_ptr_t value, bool valid) {
	throw InternalException("FilterValue called on a filter executor that does not filter single values");
}

void ExpressionFilterExecutor::FilterMask(Vector &vector, idx_t count, uint64_t *mask) {
	throw InternalException("FilterMask called on a filter executor that does not filter masks");
}

static constexpr idx_t MASK_WORDS = STANDARD_VECTOR_SIZE / 64;

static idx_t MaskToSelection(const uint64_t *mask, idx_t count, sel_t *sel) {
	idx_t result_count = 0;
	for (idx_t w = 0; w < (count + 63) / 64; w++) {
		for (auto bits = mask[w]; bits != 0; bits &= bits - 1) {
			sel[result_count++] = UnsafeNumericCast<sel_t>(w * 64 + CountZeros<uint64_t>::Trailing(bits));
		}
	}
	return result_count;
}

class ConjunctionAndFilterExecutor final : public ExpressionFilterExecutor {
public:
	explicit ConjunctionAndFilterExecutor(vector<unique_ptr<ExpressionFilterExecutor>> children_p)
	    : children(std::move(children_p)) {
		filters_values = true;
		filters_masks = true;
		for (auto &child : children) {
			filters_values = filters_values && child->FiltersValues();
			filters_masks = filters_masks && child->FiltersMasks();
		}
	}

	idx_t FilterSelection(SelectionVector &sel, Vector &vector, idx_t scan_count,
	                      idx_t &approved_tuple_count) override {
		for (auto &child : children) {
			child->FilterSelection(sel, vector, scan_count, approved_tuple_count);
			if (approved_tuple_count == 0) {
				break;
			}
		}
		return approved_tuple_count;
	}

	bool FiltersMasks() const override {
		return filters_masks;
	}

	void FilterMask(Vector &vector, idx_t count, uint64_t *mask) override {
		for (auto &child : children) {
			child->FilterMask(vector, count, mask);
		}
	}

	bool FiltersValues() const override {
		return filters_values;
	}

	bool FilterValue(const_data_ptr_t value, bool valid) override {
		for (auto &child : children) {
			if (!child->FilterValue(value, valid)) {
				return false;
			}
		}
		return true;
	}

private:
	vector<unique_ptr<ExpressionFilterExecutor>> children;
	bool filters_values;
	bool filters_masks;
};

class OptionalFilterExecutor final : public ExpressionFilterExecutor {
public:
	idx_t FilterSelection(SelectionVector &sel, Vector &vector, idx_t scan_count,
	                      idx_t &approved_tuple_count) override {
		return approved_tuple_count;
	}

	bool FiltersValues() const override {
		return true;
	}

	bool FilterValue(const_data_ptr_t value, bool valid) override {
		return true;
	}
};

template <class T, class FUNC>
static auto DispatchComparison(ExpressionType comparison, FUNC &func) {
	switch (comparison) {
	case ExpressionType::COMPARE_EQUAL:
		return func.template operator()<T, Equals>();
	case ExpressionType::COMPARE_NOTEQUAL:
		return func.template operator()<T, NotEquals>();
	case ExpressionType::COMPARE_GREATERTHAN:
		return func.template operator()<T, GreaterThan>();
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return func.template operator()<T, GreaterThanEquals>();
	case ExpressionType::COMPARE_LESSTHAN:
		return func.template operator()<T, LessThan>();
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return func.template operator()<T, LessThanEquals>();
	default:
		throw InternalException("Unsupported comparison type for comparison filter executor");
	}
}

template <bool STRINGS = true, class FUNC>
static auto DispatchComparison(PhysicalType type, ExpressionType comparison, FUNC &&func) {
	return VisitPhysicalType<STRINGS>(type, [&]<class T>() { return DispatchComparison<T>(comparison, func); });
}

class ComparisonFilterExecutor final : public ExpressionFilterExecutor {
public:
	ComparisonFilterExecutor(ExpressionType comparison_type_p, Value constant_p)
	    : comparison_type(comparison_type_p), constant(std::move(constant_p)),
	      compare_value(
	          DispatchComparison(constant.type().InternalType(), comparison_type,
	                             []<class T, class OP>() -> compare_value_t { return CompareValue<T, OP>; })) {
	}

	bool FiltersValues() const override {
		return true;
	}

	bool FilterValue(const_data_ptr_t value, bool valid) override {
		return valid && !constant.IsNull() && compare_value(value, constant);
	}

	void SetConstant(Value constant_p) {
		D_ASSERT(constant_p.type() == constant.type());
		constant = std::move(constant_p);
	}

	idx_t FilterSelection(SelectionVector &sel, Vector &vector, idx_t scan_count,
	                      idx_t &approved_tuple_count) override {
		(void)scan_count;
		if (approved_tuple_count == 0) {
			return 0;
		}
		PrepareCapacity(approved_tuple_count);
		const auto before_count = approved_tuple_count;
		if (constant.IsNull()) {
			approved_tuple_count = 0;
		} else if (!sel.IsSet() && vector.GetVectorType() == VectorType::FLAT_VECTOR &&
		           approved_tuple_count <= STANDARD_VECTOR_SIZE) {
			uint64_t mask[MASK_WORDS];
			std::fill_n(mask, (approved_tuple_count + 63) / 64, ~uint64_t(0));
			DispatchComparison(constant.type().InternalType(), comparison_type,
			                   [&]<class T, class OP>() { MaskFlat<T, OP>(vector, approved_tuple_count, mask); });
			approved_tuple_count = MaskToSelection(mask, approved_tuple_count, result_sel.data());
		} else {
			UnifiedVectorFormat vdata;
			vector.ToUnifiedFormat(vdata);
			auto &current_sel = sel.IsSet() ? sel : *FlatVector::IncrementalSelectionVector();
			approved_tuple_count =
			    DispatchComparison(constant.type().InternalType(), comparison_type, [&]<class T, class OP>() {
				    const auto predicate = constant.GetValueUnsafe<T>();
				    if (vdata.validity.CannotHaveNull()) {
					    return TemplatedSelect<T, OP, false>(vdata, predicate, current_sel, approved_tuple_count);
				    }
				    return TemplatedSelect<T, OP, true>(vdata, predicate, current_sel, approved_tuple_count);
			    });
		}
		if (approved_tuple_count != before_count) {
			sel.Initialize(result_sel);
		}
		return approved_tuple_count;
	}

	bool FiltersMasks() const override {
		return !constant.IsNull() && TypeIsConstantSize(constant.type().InternalType());
	}

	void FilterMask(Vector &vector, idx_t count, uint64_t *mask) override {
		D_ASSERT(vector.GetVectorType() == VectorType::FLAT_VECTOR && FiltersMasks());
		DispatchComparison<false>(constant.type().InternalType(), comparison_type,
		                          [&]<class T, class OP>() { MaskFlat<T, OP>(vector, count, mask); });
	}

private:
	template <class T, class OP>
	void MaskFlat(const Vector &vector, idx_t count, uint64_t *mask) const {
		using STORED = std::conditional_t<std::is_same_v<T, bool>, uint8_t, T>;
		const auto data = FlatVector::GetData<STORED>(vector);
		const auto validity = FlatVector::Validity(vector).GetData();
		const auto predicate = constant.GetValueUnsafe<T>();
		for (idx_t first = 0; first < count; first += 64) {
			const auto rows = MinValue<idx_t>(64, count - first);
			const auto in_rows = rows == 64 ? ~uint64_t(0) : (uint64_t(1) << rows) - 1;
			const auto valid = validity ? validity[first / 64] & in_rows : in_rows;
			uint64_t bits = 0;
			if (valid == in_rows) {
				for (idx_t j = 0; j < rows; j++) {
					bits |= uint64_t(OP::Operation(static_cast<T>(data[first + j]), predicate)) << j;
				}
			} else {
				for (auto rest = valid; rest != 0; rest &= rest - 1) {
					const auto j = std::countr_zero(rest);
					bits |= uint64_t(OP::Operation(static_cast<T>(data[first + j]), predicate)) << j;
				}
			}
			mask[first / 64] &= bits;
		}
	}

	template <class T, class OP, bool CAN_HAVE_NULL>
	idx_t TemplatedSelect(const UnifiedVectorFormat &vdata, T predicate, const SelectionVector &sel,
	                      idx_t approved_tuple_count) {
		auto data = UnifiedVectorFormat::GetData<T>(vdata);
		auto &mask = vdata.validity;
		idx_t result_count = 0;
		for (idx_t i = 0; i < approved_tuple_count; i++) {
			auto idx = sel.get_index(i);
			auto vector_idx = vdata.sel->get_index(idx);
			bool comparison_result =
			    (!CAN_HAVE_NULL || mask.RowIsValid(vector_idx)) && OP::Operation(data[vector_idx], predicate);
			result_sel.set_index(result_count, idx);
			result_count += comparison_result;
		}
		return result_count;
	}

	void PrepareCapacity(idx_t count) {
		if (current_capacity >= count) {
			return;
		}
		result_sel.Initialize(count);
		current_capacity = count;
	}

	using compare_value_t = bool (*)(const_data_ptr_t value, const Value &constant);

	template <class T, class OP>
	static bool CompareValue(const_data_ptr_t value, const Value &constant) {
		return OP::Operation(Load<T>(value), constant.GetValueUnsafe<T>());
	}

	ExpressionType comparison_type;
	Value constant;
	SelectionVector result_sel;
	idx_t current_capacity = 0;
	compare_value_t compare_value;
};

class DynamicFilterExecutor final : public ExpressionFilterExecutor {
public:
	DynamicFilterExecutor(shared_ptr<DynamicFilterData> filter_data_p, LogicalType column_type_p)
	    : filter_data(std::move(filter_data_p)), column_type(std::move(column_type_p)) {
	}

	idx_t FilterSelection(SelectionVector &sel, Vector &vector, idx_t scan_count,
	                      idx_t &approved_tuple_count) override {
		Refresh();
		if (!comparison) {
			return approved_tuple_count;
		}
		return comparison->FilterSelection(sel, vector, scan_count, approved_tuple_count);
	}

	bool FiltersValues() const override {
		return true;
	}

	bool FilterValue(const_data_ptr_t value, bool valid) override {
		Refresh();
		return !comparison || comparison->FilterValue(value, valid);
	}

private:
	void Refresh() {
		if (filter_data->version.load(std::memory_order_acquire) == version) {
			return;
		}
		Value constant;
		{
			lock_guard<mutex> guard(filter_data->lock);
			constant = filter_data->constant;
			version = filter_data->version.load(std::memory_order_relaxed);
		}
		if (version == 0 || constant.type() != column_type) {
			comparison.reset();
		} else if (comparison) {
			comparison->SetConstant(std::move(constant));
		} else {
			comparison = make_uniq<ComparisonFilterExecutor>(filter_data->comparison_type, std::move(constant));
		}
	}

	shared_ptr<DynamicFilterData> filter_data;
	LogicalType column_type;
	idx_t version = 0;
	unique_ptr<ComparisonFilterExecutor> comparison;
};

template <bool IS_NULL>
class NullFilterExecutor final : public ExpressionFilterExecutor {
public:
	idx_t FilterSelection(SelectionVector &sel, Vector &vector, idx_t scan_count,
	                      idx_t &approved_tuple_count) override {
		if (current_capacity < approved_tuple_count) {
			result_sel.Initialize(approved_tuple_count);
			current_capacity = approved_tuple_count;
		}
		UnifiedVectorFormat vdata;
		vector.ToUnifiedFormat(vdata);
		if (vdata.validity.CannotHaveNull()) {
			if (IS_NULL) {
				approved_tuple_count = 0;
				sel.Initialize(result_sel);
			}
			return approved_tuple_count;
		}
		auto &current_sel = sel.IsSet() ? sel : *FlatVector::IncrementalSelectionVector();
		idx_t result_count = 0;
		for (idx_t i = 0; i < approved_tuple_count; i++) {
			const auto idx = current_sel.get_index(i);
			result_sel.set_index(result_count, idx);
			result_count += vdata.validity.RowIsValid(vdata.sel->get_index(idx)) != IS_NULL;
		}
		if (result_count != approved_tuple_count) {
			sel.Initialize(result_sel);
			approved_tuple_count = result_count;
		}
		return approved_tuple_count;
	}

	bool FiltersValues() const override {
		return true;
	}

	bool FilterValue(const_data_ptr_t value, bool valid) override {
		return valid != IS_NULL;
	}

private:
	SelectionVector result_sel;
	idx_t current_capacity = 0;
};

class SelectivityOptionalFilterExecutor final : public ExpressionFilterExecutor {
public:
	SelectivityOptionalFilterExecutor(unique_ptr<ExpressionFilterExecutor> child_p, idx_t n_vectors_to_check,
	                                  float selectivity_threshold)
	    : child(std::move(child_p)), stats(n_vectors_to_check, selectivity_threshold) {
	}

	idx_t FilterSelection(SelectionVector &sel, Vector &vector, idx_t scan_count,
	                      idx_t &approved_tuple_count) override {
		if (approved_tuple_count == 0) {
			return 0;
		}
		if (!child) {
			return approved_tuple_count;
		}
		if (!stats.IsActive()) {
			stats.Update(0, 0);
			return approved_tuple_count;
		}
		const auto before_count = approved_tuple_count;
		child->FilterSelection(sel, vector, scan_count, approved_tuple_count);
		stats.Update(approved_tuple_count, before_count);
		return approved_tuple_count;
	}

	bool FiltersValues() const override {
		return !child || child->FiltersValues();
	}

	bool FilterValue(const_data_ptr_t value, bool valid) override {
		return !child || child->FilterValue(value, valid);
	}

private:
	unique_ptr<ExpressionFilterExecutor> child;
	SelectivityOptionalFilterState::SelectivityStats stats;
};

class BloomFilterExecutor final : public ExpressionFilterExecutor {
public:
	BloomFilterExecutor(const BloomFilterFunctionData &data, bool inside_selectivity_optional)
	    : filter(data.filter), filters_null_values(data.filters_null_values), hashes(LogicalType::HASH) {
		if (!inside_selectivity_optional && data.n_vectors_to_check != 0) {
			stats = make_uniq<SelectivityOptionalFilterState::SelectivityStats>(data.n_vectors_to_check,
			                                                                    data.selectivity_threshold);
		}
	}

	idx_t FilterSelection(SelectionVector &sel, Vector &vector, idx_t scan_count,
	                      idx_t &approved_tuple_count) override {
		if (approved_tuple_count == 0) {
			return 0;
		}
		if (!filter || !filter->IsInitialized()) {
			return approved_tuple_count;
		}
		if (stats && !stats->IsActive()) {
			stats->Update(0, 0);
			return approved_tuple_count;
		}

		const auto hash_capacity = sel.IsSet() ? scan_count : approved_tuple_count;
		PrepareCapacity(hash_capacity);
		if (sel.IsSet()) {
			VectorOperations::Hash(vector, hashes, sel, approved_tuple_count);
			if (hashes.GetVectorType() == VectorType::FLAT_VECTOR) {
				FlatVector::SetSize(hashes, count_t(hash_capacity));
			}
		} else {
			VectorOperations::Hash(vector, hashes, approved_tuple_count);
		}

		optional_ptr<const SelectionVector> active_sel = sel.IsSet() ? &sel : nullptr;
		const auto bloom_count = ProbeBloomFilter(active_sel, approved_tuple_count);
		if (filters_null_values || bloom_count == approved_tuple_count) {
			if (stats) {
				stats->Update(bloom_count, approved_tuple_count);
			}
			TranslateSelection(sel, bloom_count, approved_tuple_count);
			approved_tuple_count = bloom_count;
			return approved_tuple_count;
		}

		auto result_count = AddNullsToBloomSelection(sel, vector, bloom_count, approved_tuple_count);
		if (stats) {
			stats->Update(result_count, approved_tuple_count);
		}
		approved_tuple_count = result_count;
		return approved_tuple_count;
	}

private:
	idx_t ProbeBloomFilter(optional_ptr<const SelectionVector> sel, idx_t approved_tuple_count) {
		if (hashes.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			const auto hash = *ConstantVector::GetData<hash_t>(hashes);
			return filter->LookupOne(hash) ? approved_tuple_count : 0;
		}
		if (hashes.GetVectorType() != VectorType::FLAT_VECTOR) {
			hashes.Flatten();
		}
		D_ASSERT(hashes.GetVectorType() == VectorType::FLAT_VECTOR);
		if (sel) {
			return filter->LookupHashes(hashes, *sel, bloom_sel, approved_tuple_count);
		}
		return filter->LookupHashes(hashes, bloom_sel, approved_tuple_count);
	}

	idx_t AddNullsToBloomSelection(SelectionVector &sel, Vector &vector, idx_t bloom_count,
	                               idx_t approved_tuple_count) {
		UnifiedVectorFormat input_data;
		vector.ToUnifiedFormat(input_data);
		if (input_data.validity.CannotHaveNull()) {
			TranslateSelection(sel, bloom_count, approved_tuple_count);
			return bloom_count;
		}

		PrepareResultCapacity(approved_tuple_count);
		idx_t result_count = 0;
		idx_t bloom_idx = 0;
		for (idx_t i = 0; i < approved_tuple_count; i++) {
			const auto matched = bloom_idx < bloom_count && bloom_sel.get_index_unsafe(bloom_idx) == i;
			if (matched) {
				bloom_idx++;
			}
			const auto idx = sel.IsSet() ? sel.get_index(i) : i;
			const auto input_idx = input_data.sel->get_index(idx);
			bool passed;
			if (!input_data.validity.RowIsValid(input_idx)) {
				passed = !filters_null_values;
			} else {
				passed = matched;
			}
			if (passed) {
				result_sel.set_index(result_count++, idx);
			}
		}
		sel.Initialize(result_sel);
		return result_count;
	}

	void PrepareCapacity(idx_t count) {
		if (current_capacity >= count) {
			return;
		}
		hashes.Initialize(VectorDataInitialization::UNINITIALIZED, count);
		bloom_sel.Initialize(count);
		current_capacity = count;
	}

	void PrepareResultCapacity(idx_t count) {
		if (result_capacity >= count) {
			return;
		}
		result_sel.Initialize(count);
		result_capacity = count;
	}

	void TranslateSelection(SelectionVector &sel, idx_t result_count, idx_t approved_tuple_count) {
		if (result_count == approved_tuple_count) {
			return;
		}
		if (sel.IsSet()) {
			for (idx_t i = 0; i < result_count; i++) {
				const auto flat_sel_idx = bloom_sel.get_index(i);
				const auto original_sel_idx = sel.get_index(flat_sel_idx);
				sel.set_index(i, original_sel_idx);
			}
		} else {
			sel.Initialize(bloom_sel);
		}
	}

	optional_ptr<BloomFilter> filter;
	bool filters_null_values;
	Vector hashes;
	SelectionVector bloom_sel;
	SelectionVector result_sel;
	idx_t current_capacity = 0;
	idx_t result_capacity = 0;
	unique_ptr<SelectivityOptionalFilterState::SelectivityStats> stats;
};

class PrefixRangeFilterExecutor final : public ExpressionFilterExecutor {
public:
	PrefixRangeFilterExecutor(const PrefixRangeFunctionData &data, bool inside_selectivity_optional)
	    : filter(data.filter), filters_null_values(data.filters_null_values) {
		if (!inside_selectivity_optional && data.n_vectors_to_check != 0) {
			stats = make_uniq<SelectivityOptionalFilterState::SelectivityStats>(data.n_vectors_to_check,
			                                                                    data.selectivity_threshold);
		}
	}

	idx_t FilterSelection(SelectionVector &sel, Vector &vector, idx_t scan_count,
	                      idx_t &approved_tuple_count) override {
		if (approved_tuple_count == 0) {
			return 0;
		}
		if (!filter || !filter->IsInitialized()) {
			return approved_tuple_count;
		}
		if (stats && !stats->IsActive()) {
			stats->Update(0, 0);
			return approved_tuple_count;
		}

		PrepareCapacity(approved_tuple_count);
		const auto has_selection = sel.IsSet();
		const auto result_count = has_selection ? filter->LookupKeys(vector, sel, local_sel, approved_tuple_count)
		                                        : filter->LookupKeys(vector, local_sel, approved_tuple_count);
		idx_t final_count;
		if (!filters_null_values && result_count != approved_tuple_count) {
			final_count = AddNullsToSelection(sel, vector, result_count, approved_tuple_count);
		} else {
			final_count = result_count;
			if (result_count != approved_tuple_count && has_selection) {
				TranslateSelection(sel, result_count);
			} else if (result_count != approved_tuple_count) {
				sel.Initialize(local_sel);
			}
		}
		if (stats) {
			stats->Update(final_count, approved_tuple_count);
		}
		approved_tuple_count = final_count;
		return approved_tuple_count;
	}

private:
	idx_t AddNullsToSelection(SelectionVector &sel, Vector &vector, idx_t filter_count, idx_t approved_tuple_count) {
		UnifiedVectorFormat input_data;
		vector.ToUnifiedFormat(input_data);
		if (input_data.validity.CannotHaveNull()) {
			if (sel.IsSet()) {
				TranslateSelection(sel, filter_count);
			} else if (filter_count != approved_tuple_count) {
				sel.Initialize(local_sel);
			}
			return filter_count;
		}

		idx_t result_count = 0;
		idx_t filter_idx = 0;
		for (idx_t i = 0; i < approved_tuple_count; i++) {
			const auto matched = filter_idx < filter_count && local_sel.get_index_unsafe(filter_idx) == i;
			filter_idx += matched;
			const auto idx = sel.IsSet() ? sel.get_index(i) : i;
			const auto input_idx = input_data.sel->get_index(idx);
			if (matched || !input_data.validity.RowIsValid(input_idx)) {
				result_sel.set_index(result_count++, idx);
			}
		}
		sel.Initialize(result_sel);
		return result_count;
	}

	void PrepareCapacity(idx_t count) {
		if (current_capacity >= count) {
			return;
		}
		local_sel.Initialize(count);
		result_sel.Initialize(count);
		current_capacity = count;
	}

	void TranslateSelection(SelectionVector &sel, idx_t result_count) {
		for (idx_t i = 0; i < result_count; i++) {
			result_sel.set_index(i, sel.get_index(local_sel.get_index(i)));
		}
		sel.Initialize(result_sel);
	}

	optional_ptr<PrefixRangeFilter> filter;
	bool filters_null_values;
	SelectionVector local_sel;
	SelectionVector result_sel;
	idx_t current_capacity = 0;
	unique_ptr<SelectivityOptionalFilterState::SelectivityStats> stats;
};

static bool IsColumnReferenceFunction(const BoundFunctionExpression &func) {
	auto &children = func.GetChildren();
	if (children.size() != 1 || children[0]->GetExpressionClass() != ExpressionClass::BOUND_REF) {
		return false;
	}
	auto &ref = children[0]->Cast<BoundReferenceExpression>();
	return ref.Index() == 0;
}

static bool IsColumnReferenceExpression(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_REF) {
		return false;
	}
	auto &ref = expr.Cast<BoundReferenceExpression>();
	return ref.Index() == 0;
}

static bool IsSupportedComparisonType(PhysicalType type) {
	switch (type) {
	case PhysicalType::BOOL:
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
	case PhysicalType::INTERVAL:
	case PhysicalType::VARCHAR:
		return true;
	default:
		return false;
	}
}

static unique_ptr<ExpressionFilterExecutor> TryCreateComparisonExecutor(const BoundFunctionExpression &func) {
	auto comparison_type = func.GetExpressionType();
	switch (comparison_type) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOTEQUAL:
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		break;
	default:
		return nullptr;
	}

	auto &left = BoundComparisonExpression::Left(func);
	auto &right = BoundComparisonExpression::Right(func);
	optional_ptr<const BoundConstantExpression> constant;
	optional_ptr<const Expression> column;
	if (IsColumnReferenceExpression(left) && right.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		column = &left;
		constant = &right.Cast<BoundConstantExpression>();
	} else if (IsColumnReferenceExpression(right) && left.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		column = &right;
		comparison_type = FlipComparisonExpression(comparison_type);
		constant = &left.Cast<BoundConstantExpression>();
	}
	if (!constant) {
		return nullptr;
	}
	const auto column_type = column->GetReturnType().InternalType();
	const auto constant_type = constant->GetValue().type().InternalType();
	if (column_type != constant_type || !IsSupportedComparisonType(column_type)) {
		return nullptr;
	}
	return make_uniq<ComparisonFilterExecutor>(comparison_type, constant->GetValue());
}

static unique_ptr<ExpressionFilterExecutor> TryCreateBetweenExecutor(const BoundFunctionExpression &func) {
	if (!BoundBetweenExpression::HasValidBindData(func) ||
	    !IsColumnReferenceExpression(BoundBetweenExpression::Input(func))) {
		return nullptr;
	}
	const auto column_type = BoundBetweenExpression::Input(func).GetReturnType().InternalType();
	if (!IsSupportedComparisonType(column_type)) {
		return nullptr;
	}
	vector<unique_ptr<ExpressionFilterExecutor>> children;
	const auto add = [&](const Expression &bound, ExpressionType comparison_type) {
		if (bound.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			return false;
		}
		const auto &constant = bound.Cast<BoundConstantExpression>().GetValue();
		if (constant.type().InternalType() != column_type) {
			return false;
		}
		children.push_back(make_uniq<ComparisonFilterExecutor>(comparison_type, constant));
		return true;
	};
	if (!add(BoundBetweenExpression::LowerBound(func), BoundBetweenExpression::LowerComparisonType(func)) ||
	    !add(BoundBetweenExpression::UpperBound(func), BoundBetweenExpression::UpperComparisonType(func))) {
		return nullptr;
	}
	return make_uniq<ConjunctionAndFilterExecutor>(std::move(children));
}

static unique_ptr<ExpressionFilterExecutor> TryCreateFunctionExecutor(const BoundFunctionExpression &func,
                                                                      bool inside_selectivity_optional) {
	if (!inside_selectivity_optional && func.GetExpressionType() == ExpressionType::COMPARE_BETWEEN) {
		return TryCreateBetweenExecutor(func);
	}
	if (!inside_selectivity_optional && func.GetChildren().size() == 2 &&
	    BoundComparisonExpression::IsComparison(func.GetExpressionType())) {
		return TryCreateComparisonExecutor(func);
	}
	if (!IsColumnReferenceFunction(func)) {
		return nullptr;
	}

	const auto &func_name = func.Function().GetName();
	if (func_name == OptionalFilterScalarFun::NAME) {
		return make_uniq<OptionalFilterExecutor>();
	}
	if (func_name == DynamicFilterScalarFun::NAME) {
		if (!func.BindInfo()) {
			return nullptr;
		}
		auto &filter_data = func.BindInfo()->Cast<DynamicFilterFunctionData>().filter_data;
		const auto &column_type = func.GetChildren()[0]->GetReturnType();
		if (!filter_data || !IsSupportedComparisonType(column_type.InternalType())) {
			return nullptr;
		}
		return make_uniq<DynamicFilterExecutor>(filter_data, column_type);
	}
	if (func_name == SelectivityOptionalFilterScalarFun::NAME) {
		auto optional_child = ExpressionFilter::GetOptionalFilterChild(func);
		if (!optional_child) {
			return make_uniq<OptionalFilterExecutor>();
		}
		auto child = TryCreateFastExecutor(*optional_child, true);
		if (!child) {
			return nullptr;
		}
		auto &data = func.BindInfo()->Cast<SelectivityOptionalFilterFunctionData>();
		return make_uniq<SelectivityOptionalFilterExecutor>(std::move(child), data.n_vectors_to_check,
		                                                    data.selectivity_threshold);
	}
	if (func_name == BloomFilterScalarFun::NAME) {
		if (!func.BindInfo()) {
			return nullptr;
		}
		return make_uniq<BloomFilterExecutor>(func.BindInfo()->Cast<BloomFilterFunctionData>(),
		                                      inside_selectivity_optional);
	}
	if (func_name == PrefixRangeScalarFun::NAME) {
		if (!func.BindInfo()) {
			return nullptr;
		}
		return make_uniq<PrefixRangeFilterExecutor>(func.BindInfo()->Cast<PrefixRangeFunctionData>(),
		                                            inside_selectivity_optional);
	}
	return nullptr;
}

static unique_ptr<ExpressionFilterExecutor> TryCreateFastExecutor(const Expression &expression,
                                                                  bool inside_selectivity_optional) {
	switch (expression.GetExpressionClass()) {
	case ExpressionClass::BOUND_FUNCTION:
		return TryCreateFunctionExecutor(expression.Cast<BoundFunctionExpression>(), inside_selectivity_optional);
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op = expression.Cast<BoundOperatorExpression>();
		if (op.GetChildren().size() != 1 || !IsColumnReferenceExpression(*op.GetChildren()[0])) {
			return nullptr;
		}
		switch (op.GetExpressionType()) {
		case ExpressionType::OPERATOR_IS_NULL:
			return make_uniq<NullFilterExecutor<true>>();
		case ExpressionType::OPERATOR_IS_NOT_NULL:
			return make_uniq<NullFilterExecutor<false>>();
		default:
			return nullptr;
		}
	}
	case ExpressionClass::BOUND_CONJUNCTION: {
		if (expression.GetExpressionType() != ExpressionType::CONJUNCTION_AND) {
			return nullptr;
		}
		auto &conjunction = expression.Cast<BoundConjunctionExpression>();
		vector<unique_ptr<ExpressionFilterExecutor>> children;
		for (auto &child_expr : conjunction.GetChildren()) {
			auto child_executor = TryCreateFastExecutor(*child_expr, inside_selectivity_optional);
			if (!child_executor) {
				return nullptr;
			}
			children.push_back(std::move(child_executor));
		}
		return make_uniq<ConjunctionAndFilterExecutor>(std::move(children));
	}
	default:
		return nullptr;
	}
}

unique_ptr<TableFilterState> TableFilterState::Initialize(ClientContext &context, const TableFilter &filter) {
	auto &expr_filter = ExpressionFilter::GetExpressionFilter(filter, "TableFilterState::Initialize");
	return make_uniq<ExpressionFilterState>(context, *expr_filter.expr);
}

} // namespace duckdb
