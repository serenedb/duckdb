#pragma once
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/string_map_set.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

#include <absl/container/flat_hash_map.h>

namespace duckdb {

static constexpr idx_t LIST_SEARCH_HASH_THRESHOLD = 64;
static constexpr idx_t LIST_SEARCH_MAP_ROWS = 64;

struct ListSearchState : public FunctionLocalState {
	explicit ListSearchState(bool constant_list) : constant_list(constant_list) {
	}

	static unique_ptr<FunctionLocalState> Init(ExpressionState &, const BoundFunctionExpression &expr, FunctionData *) {
		return make_uniq<ListSearchState>(expr.GetChildren()[0]->IsFoldable());
	}

	static ListSearchState &Get(ExpressionState &state) {
		return ExecuteFunctionState::GetFunctionState(state)->Cast<ListSearchState>();
	}

	bool UseMap(const Vector &list_vec, const PhysicalType type, const idx_t rows) {
		if (list_vec.GetVectorType() != VectorType::CONSTANT_VECTOR || type == PhysicalType::INTERVAL) {
			return false;
		}
		if (built || type == PhysicalType::STRUCT || type == PhysicalType::LIST || type == PhysicalType::ARRAY ||
		    ConstantVector::IsNull(list_vec)) {
			return true;
		}
		if (ConstantVector::GetData<list_entry_t>(list_vec)[0].length < LIST_SEARCH_HASH_THRESHOLD) {
			return false;
		}
		const auto searched = constant_list ? scanned_rows + rows : rows;
		if (searched >= LIST_SEARCH_MAP_ROWS) {
			return true;
		}
		if (constant_list) {
			scanned_rows = searched;
		}
		return false;
	}

	void Build(const Vector &list_vec, const Vector &source_vec) {
		const auto list = ConstantVector::GetData<list_entry_t>(list_vec)[0];
		Vector elements(source_vec, list.offset, list.offset + list.length);
		const OrderModifiers order_modifiers(OrderType::ASCENDING, OrderByNullType::NULLS_LAST);
		positions.clear();
		keys = make_uniq<Vector>(LogicalType::BLOB, list.length);
		CreateSortKeyHelpers::CreateSortKey(elements, list.length, order_modifiers, *keys);
		positions.reserve(list.length);
		null_position = 0;
		const auto element_validity = elements.Validity();
		const auto element_data = keys->Values<string_t>();
		for (idx_t i = 0; i < list.length; i++) {
			const auto position = UnsafeNumericCast<int32_t>(i + 1);
			if (element_validity.IsValid(i)) {
				positions.try_emplace(element_data[i].GetValue(), position);
			} else if (null_position == 0) {
				null_position = position;
			}
		}
		built = constant_list;
	}

	const bool constant_list;
	bool built = false;
	idx_t scanned_rows = 0;
	unique_ptr<Vector> keys;
	absl::flat_hash_map<string_t, int32_t, StringHash, StringEquality> positions;
	int32_t null_position = 0;
};

template <class RETURN_TYPE, bool FIND_NULLS>
idx_t ListSearchMapOp(const Vector &list_vec, const Vector &source_vec, const Vector &target_vec, Vector &result_vec,
                      const idx_t target_count, ListSearchState &state) {
	constexpr bool return_pos = std::is_same<RETURN_TYPE, int32_t>::value;
	result_vec.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::Writer<RETURN_TYPE>(result_vec, target_count);
	if (ConstantVector::IsNull(list_vec)) {
		for (idx_t row_idx = 0; row_idx < target_count; row_idx++) {
			result_data.WriteNull();
		}
		return 0;
	}
	if (!state.built) {
		state.Build(list_vec, source_vec);
	}

	const OrderModifiers order_modifiers(OrderType::ASCENDING, OrderByNullType::NULLS_LAST);
	Vector target_keys(LogicalType::BLOB, target_count);
	CreateSortKeyHelpers::CreateSortKey(target_vec, target_count, order_modifiers, target_keys);

	idx_t total_matches = 0;
	const auto target_validity = target_vec.Validity();
	const auto target_data = target_keys.Values<string_t>();
	for (idx_t row_idx = 0; row_idx < target_count; row_idx++) {
		int32_t position = 0;
		if (target_validity.IsValid(row_idx)) {
			auto entry = state.positions.find(target_data[row_idx].GetValue());
			if (entry != state.positions.end()) {
				position = entry->second;
			}
		} else if (!FIND_NULLS) {
			result_data.WriteNull();
			continue;
		} else {
			position = state.null_position;
		}
		if (position == 0) {
			if constexpr (return_pos) {
				result_data.WriteNull();
			} else {
				result_data.WriteValue(RETURN_TYPE(false));
			}
			continue;
		}
		total_matches++;
		if constexpr (return_pos) {
			result_data.WriteValue(position);
		} else {
			result_data.WriteValue(RETURN_TYPE(true));
		}
	}
	return total_matches;
}

template <class T, class RETURN_TYPE, bool FIND_NULLS>
idx_t ListSearchSimpleOp(const Vector &input_list, const Vector &list_child, const Vector &target, Vector &result,
                         const idx_t count) {
	// If the return type is not a bool, return the position
	const auto return_pos = std::is_same<RETURN_TYPE, int32_t>::value;

	const auto list_entries = input_list.Values<list_entry_t>();
	const auto child_data = list_child.Values<T>();
	const auto target_data = target.Values<T>();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::Writer<RETURN_TYPE>(result, count);

	idx_t total_matches = 0;

	for (idx_t row_idx = 0; row_idx < count; ++row_idx) {
		auto list_entry = list_entries[row_idx];

		// The entire list is NULL, the result is also NULL.
		if (!list_entry.IsValid()) {
			result_data.WriteNull();
			continue;
		}
		auto list = list_entry.GetValue();
		auto target_entry = target_data[row_idx];

		const bool target_valid = target_entry.IsValid();

		// We are finished, if we are not looking for NULL, and the target is NULL.
		const auto finished = !FIND_NULLS && !target_valid;
		// We did not find the target (finished, or list is empty).
		if (finished || list.length == 0) {
			if (finished || return_pos) {
				// Return NULL as the position.
				result_data.WriteNull();
			} else {
				// Set 'contains' to false.
				result_data.WriteValue(RETURN_TYPE(false));
			}
			continue;
		}

		const auto entry_length = list.length;
		const auto entry_offset = list.offset;

		bool found = false;
		RETURN_TYPE found_value {};

		for (auto list_idx = entry_offset; list_idx < entry_length + entry_offset && !found; list_idx++) {
			auto child_entry = child_data[list_idx];
			const bool child_valid = child_entry.IsValid();

			if ((FIND_NULLS && !child_valid && !target_valid) ||
			    (child_valid && target_valid &&
			     Equals::Operation<T>(child_entry.GetValue(), target_entry.GetValue()))) {
				found = true;
				total_matches++;
				if (return_pos) {
					found_value = UnsafeNumericCast<int32_t>(1 + list_idx - entry_offset);
				} else {
					found_value = RETURN_TYPE(true);
				}
			}
		}

		if (!found) {
			if (return_pos) {
				result_data.WriteNull();
			} else {
				result_data.WriteValue(RETURN_TYPE(false));
			}
		} else {
			result_data.WriteValue(found_value);
		}
	}

	return total_matches;
}

template <class RETURN_TYPE, bool FIND_NULLS>
idx_t ListSearchNestedOp(const Vector &list_vec, const Vector &source_vec, const Vector &target_vec, Vector &result_vec,
                         const idx_t target_count) {
	// Set up sort keys for nested types.
	auto source_count = ListVector::GetListSize(list_vec);
	Vector source_sort_key_vec(LogicalType::BLOB, source_count);
	Vector target_sort_key_vec(LogicalType::BLOB, target_count);

	const OrderModifiers order_modifiers(OrderType::ASCENDING, OrderByNullType::NULLS_LAST);
	// Pass explicit counts: source_vec and target_vec may have different sizes than source_count/target_count
	// when called via dictionary expression optimization (TryExecuteDictionaryExpression uses the dictionary
	// size as the chunk count, while the non-key arguments retain their original size).
	CreateSortKeyHelpers::CreateSortKeyWithValidity(source_vec, source_sort_key_vec, order_modifiers, source_count);
	CreateSortKeyHelpers::CreateSortKeyWithValidity(target_vec, target_sort_key_vec, order_modifiers, target_count);

	return ListSearchSimpleOp<string_t, RETURN_TYPE, FIND_NULLS>(list_vec, source_sort_key_vec, target_sort_key_vec,
	                                                             result_vec, target_count);
}

//! "Search" each list in the list vector for the corresponding value in the target vector, returning either
//! true/false or the position of the value in the list. The result vector is populated with the result of the search.
//! usually the "source" vector is the list child vector, but it is passed separately to enable searching nested
//! children, for example when searching the keys of a MAP vectors.
template <class RETURN_TYPE, bool FIND_NULLS = false>
idx_t ListSearchOp(const Vector &list_v, const Vector &source_v, const Vector &target_v, Vector &result_v,
                   idx_t target_count, ListSearchState &state) {
	const auto type = target_v.GetType().InternalType();
	if (state.UseMap(list_v, type, target_count)) {
		return ListSearchMapOp<RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v, target_count, state);
	}
	switch (type) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return ListSearchSimpleOp<int8_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v, target_count);
	case PhysicalType::INT16:
		return ListSearchSimpleOp<int16_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v, target_count);
	case PhysicalType::INT32:
		return ListSearchSimpleOp<int32_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v, target_count);
	case PhysicalType::INT64:
		return ListSearchSimpleOp<int64_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v, target_count);
	case PhysicalType::INT128:
		return ListSearchSimpleOp<hugeint_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v,
		                                                              target_count);
	case PhysicalType::UINT8:
		return ListSearchSimpleOp<uint8_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v, target_count);
	case PhysicalType::UINT16:
		return ListSearchSimpleOp<uint16_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v,
		                                                             target_count);
	case PhysicalType::UINT32:
		return ListSearchSimpleOp<uint32_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v,
		                                                             target_count);
	case PhysicalType::UINT64:
		return ListSearchSimpleOp<uint64_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v,
		                                                             target_count);
	case PhysicalType::UINT128:
		return ListSearchSimpleOp<uhugeint_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v,
		                                                               target_count);
	case PhysicalType::FLOAT:
		return ListSearchSimpleOp<float, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v, target_count);
	case PhysicalType::DOUBLE:
		return ListSearchSimpleOp<double, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v, target_count);
	case PhysicalType::VARCHAR:
		return ListSearchSimpleOp<string_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v,
		                                                             target_count);
	case PhysicalType::INTERVAL:
		return ListSearchSimpleOp<interval_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v,
		                                                               target_count);
	case PhysicalType::STRUCT:
	case PhysicalType::LIST:
	case PhysicalType::ARRAY:
		return ListSearchNestedOp<RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v, target_count);
	default:
		throw NotImplementedException("This function has not been implemented for logical type %s",
		                              TypeIdToString(type));
	}
}

} // namespace duckdb
