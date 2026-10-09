#pragma once

#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/function/scalar_function.hpp"

#include <type_traits>

namespace duckdb {

struct AddWrappingOperator {
	template <class T>
	static inline T Operation(T left, T right, bool &overflow) {
		using UNSIGNED = std::make_unsigned_t<T>;
		auto result = static_cast<T>(static_cast<UNSIGNED>(left) + static_cast<UNSIGNED>(right));
		if constexpr (std::is_signed_v<T>) {
			overflow |= ((left ^ result) & (right ^ result)) < 0;
		} else {
			overflow |= result < left;
		}
		return result;
	}

	template <class T>
	static inline T DenseOperation(T left, T right, bool &overflow) {
		return Operation(left, right, overflow);
	}
};

struct SubtractWrappingOperator {
	template <class T>
	static inline T Operation(T left, T right, bool &overflow) {
		using UNSIGNED = std::make_unsigned_t<T>;
		auto result = static_cast<T>(static_cast<UNSIGNED>(left) - static_cast<UNSIGNED>(right));
		if constexpr (std::is_signed_v<T>) {
			overflow |= ((left ^ right) & (left ^ result)) < 0;
		} else {
			overflow |= left < right;
		}
		return result;
	}

	template <class T>
	static inline T DenseOperation(T left, T right, bool &overflow) {
		return Operation(left, right, overflow);
	}
};

struct MultiplyWrappingOperator {
	template <class T>
	static inline T Operation(T left, T right, bool &overflow) {
		T result;
#if (__GNUC__ >= 5) || defined(__clang__)
		overflow |= __builtin_mul_overflow(left, right, &result);
#else
		overflow |= !TryMultiplyOperator::Operation(left, right, result);
#endif
		return result;
	}

	template <class T>
	static inline T DenseOperation(T left, T right, bool &overflow) {
		if constexpr (std::is_signed_v<T> && sizeof(T) <= sizeof(int32_t)) {
			using WIDE = std::conditional_t<sizeof(T) == sizeof(int32_t), int64_t, int32_t>;
			auto product = static_cast<WIDE>(left) * static_cast<WIDE>(right);
			auto result = static_cast<T>(product);
			overflow |= static_cast<WIDE>(result) != product;
			return result;
		} else {
			return Operation(left, right, overflow);
		}
	}
};

template <class CHECKED_OP>
struct WrappingOperator;

template <>
struct WrappingOperator<AddOperatorOverflowCheck> {
	using type = AddWrappingOperator;
};

template <>
struct WrappingOperator<SubtractOperatorOverflowCheck> {
	using type = SubtractWrappingOperator;
};

template <>
struct WrappingOperator<MultiplyOperatorOverflowCheck> {
	using type = MultiplyWrappingOperator;
};

template <class T, class WRAPPING_OP, bool LEFT_CONSTANT, bool RIGHT_CONSTANT>
bool OverflowCheckFlatLoop(const T *__restrict left_data, const T *__restrict right_data, T *__restrict result_data,
                           const ValidityMask &mask, idx_t count) {
	bool overflow = false;
	auto compute_row = [&](idx_t row) {
		result_data[row] = WRAPPING_OP::DenseOperation(left_data[LEFT_CONSTANT ? 0 : row],
		                                               right_data[RIGHT_CONSTANT ? 0 : row], overflow);
	};
	if (!mask.CanHaveNull()) {
		for (idx_t row = 0; row < count; row++) {
			compute_row(row);
		}
		return overflow;
	}
	idx_t row = 0;
	auto entry_count = ValidityMask::EntryCount(count);
	for (idx_t entry_idx = 0; entry_idx < entry_count; entry_idx++) {
		auto validity_entry = mask.GetValidityEntry(entry_idx);
		auto entry_start = row;
		auto entry_end = MinValue<idx_t>(entry_start + ValidityMask::BITS_PER_VALUE, count);
		if (ValidityMask::AllValid(validity_entry)) {
			for (; row < entry_end; row++) {
				compute_row(row);
			}
		} else if (ValidityMask::NoneValid(validity_entry)) {
			row = entry_end;
		} else {
			for (; row < entry_end; row++) {
				if (ValidityMask::RowIsValid(validity_entry, row - entry_start)) {
					compute_row(row);
				}
			}
		}
	}
	return overflow;
}

inline bool IsFlatOrNonNullConstant(const Vector &input) {
	switch (input.GetVectorType()) {
	case VectorType::FLAT_VECTOR:
		return true;
	case VectorType::CONSTANT_VECTOR:
		return !ConstantVector::IsNull(input);
	default:
		return false;
	}
}

template <class T, class WRAPPING_OP>
bool TryOverflowCheckFlat(const Vector &left, const Vector &right, Vector &result, idx_t count, bool &overflow) {
	if (!IsFlatOrNonNullConstant(left) || !IsFlatOrNonNullConstant(right)) {
		return false;
	}
	auto left_constant = left.GetVectorType() == VectorType::CONSTANT_VECTOR;
	auto right_constant = right.GetVectorType() == VectorType::CONSTANT_VECTOR;
	if (left_constant && right_constant) {
		return false;
	}

	result.SetVectorType(VectorType::FLAT_VECTOR);
	if (result.size() != count) {
		FlatVector::SetSize(result, count);
	}
	auto &mask = FlatVector::ValidityMutable(result);
	mask.Reset(count);
	if (!left_constant) {
		mask.Combine(FlatVector::Validity(left), count);
	}
	if (!right_constant) {
		mask.Combine(FlatVector::Validity(right), count);
	}

	auto left_data = left_constant ? ConstantVector::GetData<T>(left) : FlatVector::GetData<T>(left);
	auto right_data = right_constant ? ConstantVector::GetData<T>(right) : FlatVector::GetData<T>(right);
	auto result_data = FlatVector::GetDataMutable<T>(result);
	if (left_constant) {
		overflow = OverflowCheckFlatLoop<T, WRAPPING_OP, true, false>(left_data, right_data, result_data, mask, count);
	} else if (right_constant) {
		overflow = OverflowCheckFlatLoop<T, WRAPPING_OP, false, true>(left_data, right_data, result_data, mask, count);
	} else {
		overflow = OverflowCheckFlatLoop<T, WRAPPING_OP, false, false>(left_data, right_data, result_data, mask, count);
	}
	return true;
}

template <class T, class CHECKED_OP>
void OverflowCheckFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	D_ASSERT(input.ColumnCount() == 2);
	using WRAPPING_OP = typename WrappingOperator<CHECKED_OP>::type;
	bool overflow = false;
	if (!TryOverflowCheckFlat<T, WRAPPING_OP>(input.data[0], input.data[1], result, input.size(), overflow)) {
		BinaryExecutor::Execute<T, T, T>(input.data[0], input.data[1], result, [&overflow](T left, T right) {
			return WRAPPING_OP::Operation(left, right, overflow);
		});
	}
	if (overflow) {
		BinaryExecutor::ExecuteStandard<T, T, T, CHECKED_OP>(input.data[0], input.data[1], result);
	}
}

template <class CHECKED_OP>
scalar_function_t GetOverflowCheckFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::INT8:
		return &OverflowCheckFunction<int8_t, CHECKED_OP>;
	case PhysicalType::INT16:
		return &OverflowCheckFunction<int16_t, CHECKED_OP>;
	case PhysicalType::INT32:
		return &OverflowCheckFunction<int32_t, CHECKED_OP>;
	case PhysicalType::INT64:
		return &OverflowCheckFunction<int64_t, CHECKED_OP>;
	case PhysicalType::UINT8:
		return &OverflowCheckFunction<uint8_t, CHECKED_OP>;
	case PhysicalType::UINT16:
		return &OverflowCheckFunction<uint16_t, CHECKED_OP>;
	case PhysicalType::UINT32:
		return &OverflowCheckFunction<uint32_t, CHECKED_OP>;
	case PhysicalType::UINT64:
		return &OverflowCheckFunction<uint64_t, CHECKED_OP>;
	case PhysicalType::INT128:
		return &ScalarFunction::BinaryFunction<hugeint_t, hugeint_t, hugeint_t, CHECKED_OP>;
	case PhysicalType::UINT128:
		return &ScalarFunction::BinaryFunction<uhugeint_t, uhugeint_t, uhugeint_t, CHECKED_OP>;
	default:
		throw NotImplementedException("Unimplemented type for GetOverflowCheckFunction: %s", TypeIdToString(type));
	}
}

} // namespace duckdb
