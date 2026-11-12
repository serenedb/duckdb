//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/core_functions/aggregate/algebraic/stddev.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/aggregate_function.hpp"
#include <complex>
#include <cmath>

namespace duckdb {

struct StddevState {
	static constexpr const char *STATE_NAMES[] = {"count", "mean", "dsquared"};
	using STATE_TYPE = StructStateType<uint64_t, double, double>;

	uint64_t count;  //  n
	double mean;     //  M1
	double dsquared; //  M2
};

// Streaming approximate standard deviation using Welford's
// method, DOI: 10.2307/1266577
struct STDDevBaseOperation {
	template <class INPUT_TYPE, class STATE>
	static void Execute(STATE &state, const INPUT_TYPE &input) {
		// update running mean and d^2
		state.count++;
		const double mean_differential = (input - state.mean) / state.count;
		const double new_mean = state.mean + mean_differential;
		const double dsquared_increment = (input - new_mean) * (input - state.mean);
		const double new_dsquared = state.dsquared + dsquared_increment;

		state.mean = new_mean;
		state.dsquared = new_dsquared;
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &) {
		Execute(state, input);
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input,
	                              idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT_TYPE, STATE, OP>(state, input, unary_input);
		}
	}

	template <class INPUT_TYPE, class STATE_TYPE, bool ALL_VALID, bool DIRECT>
	static void ClusteredOpInternal(STATE_TYPE &state, const INPUT_TYPE *vals, const sel_t *sel,
	                                const SelectionVector &isel, const ValidityMask &validity, idx_t pos, idx_t end) {
		auto index = [&](idx_t k) {
			return DIRECT ? k : isel.get_index(sel ? sel[k] : k);
		};
		double sum = 0;
		uint64_t count = 0;
		for (idx_t k = pos; k < end; k++) {
			auto idx = index(k);
			if (ALL_VALID || validity.RowIsValidUnsafe(idx)) {
				sum += static_cast<double>(vals[idx]);
				count++;
			}
		}
		if (count == 0) {
			return;
		}
		const double mean = sum / static_cast<double>(count);
		double dsquared = 0;
		for (idx_t k = pos; k < end; k++) {
			auto idx = index(k);
			if (ALL_VALID || validity.RowIsValidUnsafe(idx)) {
				const double delta = static_cast<double>(vals[idx]) - mean;
				dsquared += delta * delta;
			}
		}
		STATE_TYPE chunk;
		chunk.count = count;
		chunk.mean = mean;
		chunk.dsquared = dsquared;
		CombineStates(chunk, state);
	}

	template <class INPUT_TYPE, class STATE_TYPE, class OP>
	static void ClusteredOp(STATE_TYPE &state, const INPUT_TYPE *vals, AggregateUnaryInput &, const sel_t *sel,
	                        const SelectionVector &isel, const ValidityMask &validity, idx_t pos, idx_t end) {
		const bool direct = !sel && !isel.IsSet();
		if (validity.CanHaveNull()) {
			if (direct) {
				ClusteredOpInternal<INPUT_TYPE, STATE_TYPE, false, true>(state, vals, sel, isel, validity, pos, end);
			} else {
				ClusteredOpInternal<INPUT_TYPE, STATE_TYPE, false, false>(state, vals, sel, isel, validity, pos, end);
			}
		} else if (direct) {
			ClusteredOpInternal<INPUT_TYPE, STATE_TYPE, true, true>(state, vals, sel, isel, validity, pos, end);
		} else {
			ClusteredOpInternal<INPUT_TYPE, STATE_TYPE, true, false>(state, vals, sel, isel, validity, pos, end);
		}
	}

	template <class STATE>
	static void CombineStates(const STATE &source, STATE &target) {
		if (target.count == 0) {
			target = source;
		} else if (source.count > 0) {
			const auto count = target.count + source.count;
			D_ASSERT(count >= target.count);
			const double target_count = static_cast<double>(target.count);
			const double source_count = static_cast<double>(source.count);
			const double total_count = static_cast<double>(count);
			const auto delta = source.mean - target.mean;
			const auto mean = std::fma(source_count / total_count, delta, target.mean);
			target.dsquared =
			    source.dsquared + target.dsquared + delta * delta * source_count * target_count / total_count;
			target.mean = mean;
			target.count = count;
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		CombineStates(source, target);
	}

	static bool IgnoreNull() {
		return true;
	}
};

struct VarSampOperation : public STDDevBaseOperation {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count <= 1) {
			finalize_data.ReturnNull();
		} else {
			target = state.dsquared / (state.count - 1);
		}
	}
};

struct VarPopOperation : public STDDevBaseOperation {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count == 0) {
			finalize_data.ReturnNull();
		} else {
			target = state.count > 1 ? (state.dsquared / state.count) : 0;
		}
	}
};

struct STDDevSampOperation : public STDDevBaseOperation {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count <= 1) {
			finalize_data.ReturnNull();
		} else {
			target = sqrt(state.dsquared / (state.count - 1));
		}
	}
};

struct STDDevPopOperation : public STDDevBaseOperation {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count == 0) {
			finalize_data.ReturnNull();
		} else {
			target = state.count > 1 ? sqrt(state.dsquared / state.count) : 0;
		}
	}
};

struct StandardErrorOfTheMeanOperation : public STDDevBaseOperation {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count == 0) {
			finalize_data.ReturnNull();
		} else {
			target = sqrt(state.dsquared / state.count) / sqrt((state.count));
		}
	}
};
} // namespace duckdb
