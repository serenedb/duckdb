#pragma once
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/algorithm.hpp"
#include <cmath>

#if defined(__clang__)
#define DUCKDB_REASSOCIATE_LOOP _Pragma("clang fp reassociate(on) contract(fast)")
#else
#define DUCKDB_REASSOCIATE_LOOP
#endif

namespace duckdb {

//-------------------------------------------------------------------------
// Folding Operations
//-------------------------------------------------------------------------
struct InnerProductOp {
	static constexpr bool ALLOW_EMPTY = true;

	template <class TYPE>
	static TYPE Operation(const TYPE *lhs_data, const TYPE *rhs_data, const idx_t count) {
		DUCKDB_REASSOCIATE_LOOP
		TYPE result = 0;

		auto lhs_ptr = lhs_data;
		auto rhs_ptr = rhs_data;

		for (idx_t i = 0; i < count; i++) {
			const auto x = *lhs_ptr++;
			const auto y = *rhs_ptr++;
			result += x * y;
		}

		return result;
	}
};

struct NegativeInnerProductOp {
	static constexpr bool ALLOW_EMPTY = true;

	template <class TYPE>
	static TYPE Operation(const TYPE *lhs_data, const TYPE *rhs_data, const idx_t count) {
		return -InnerProductOp::Operation(lhs_data, rhs_data, count);
	}
};

struct CosineSimilarityOp {
	static constexpr bool ALLOW_EMPTY = false;

	template <class TYPE>
	static TYPE Operation(const TYPE *lhs_data, const TYPE *rhs_data, const idx_t count) {
		DUCKDB_REASSOCIATE_LOOP
		TYPE distance = 0;
		TYPE norm_l = 0;
		TYPE norm_r = 0;

		auto l_ptr = lhs_data;
		auto r_ptr = rhs_data;

		for (idx_t i = 0; i < count; i++) {
			const auto x = *l_ptr++;
			const auto y = *r_ptr++;
			distance += x * y;
			norm_l += x * x;
			norm_r += y * y;
		}

		const auto denominator = std::sqrt(static_cast<double>(norm_l) * static_cast<double>(norm_r));
		if (denominator == 0) {
			return 0;
		}
		auto similarity = static_cast<TYPE>(distance / denominator);
		return std::max(static_cast<TYPE>(-1.0), std::min(similarity, static_cast<TYPE>(1.0)));
	}
};

struct CosineDistanceOp {
	static constexpr bool ALLOW_EMPTY = false;

	template <class TYPE>
	static TYPE Operation(const TYPE *lhs_data, const TYPE *rhs_data, const idx_t count) {
		return static_cast<TYPE>(1.0) - CosineSimilarityOp::Operation(lhs_data, rhs_data, count);
	}
};

struct DistanceSquaredOp {
	static constexpr bool ALLOW_EMPTY = true;

	template <class TYPE>
	static TYPE Operation(const TYPE *lhs_data, const TYPE *rhs_data, const idx_t count) {
		DUCKDB_REASSOCIATE_LOOP
		TYPE distance = 0;

		auto l_ptr = lhs_data;
		auto r_ptr = rhs_data;

		for (idx_t i = 0; i < count; i++) {
			const auto x = *l_ptr++;
			const auto y = *r_ptr++;
			const auto diff = x - y;
			distance += diff * diff;
		}

		return distance;
	}
};

struct DistanceOp {
	static constexpr bool ALLOW_EMPTY = true;

	template <class TYPE>
	static TYPE Operation(const TYPE *lhs_data, const TYPE *rhs_data, const idx_t count) {
		return std::sqrt(DistanceSquaredOp::Operation(lhs_data, rhs_data, count));
	}
};

struct L1DistanceOp {
	static constexpr bool ALLOW_EMPTY = true;

	template <class TYPE>
	static TYPE Operation(const TYPE *lhs_data, const TYPE *rhs_data, const idx_t count) {
		DUCKDB_REASSOCIATE_LOOP
		TYPE distance = 0;
		for (idx_t i = 0; i < count; i++) {
			distance += std::abs(lhs_data[i] - rhs_data[i]);
		}
		return distance;
	}
};

struct L1NormOp {
	template <class TYPE>
	static TYPE Operation(const TYPE *data, const idx_t count) {
		DUCKDB_REASSOCIATE_LOOP
		TYPE norm = 0;
		for (idx_t i = 0; i < count; i++) {
			norm += std::abs(data[i]);
		}
		return norm;
	}
};

struct NormSquaredOp {
	template <class TYPE>
	static TYPE Operation(const TYPE *data, const idx_t count) {
		DUCKDB_REASSOCIATE_LOOP
		TYPE norm = 0;
		for (idx_t i = 0; i < count; i++) {
			norm += data[i] * data[i];
		}
		return norm;
	}
};

struct L2NormOp {
	template <class TYPE>
	static TYPE Operation(const TYPE *data, const idx_t count) {
		return std::sqrt(NormSquaredOp::Operation(data, count));
	}
};

template <class NORM>
struct NormalizeOp {
	template <class TYPE>
	static void Operation(const TYPE *data, TYPE *result, const idx_t count) {
		const auto norm = NORM::Operation(data, count);
		if (norm == 0) {
			for (idx_t i = 0; i < count; i++) {
				result[i] = 0;
			}
			return;
		}
		const auto inverse = static_cast<TYPE>(1) / norm;
		for (idx_t i = 0; i < count; i++) {
			result[i] = data[i] * inverse;
		}
	}
};

using L1NormalizeOp = NormalizeOp<L1NormOp>;
using L2NormalizeOp = NormalizeOp<L2NormOp>;

} // namespace duckdb
