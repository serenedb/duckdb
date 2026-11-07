#include "core_functions/aggregate/distributive_functions.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/common/algorithm.hpp"

namespace duckdb {

namespace {

struct KurtosisState {
	static constexpr const char *STATE_NAMES[] = {"n", "shift", "sum", "sum_sqr", "sum_cub", "sum_four"};
	using STATE_TYPE = StructStateType<idx_t, double, double, double, double, double>;

	idx_t n;
	double shift;
	double sum;
	double sum_sqr;
	double sum_cub;
	double sum_four;
};

struct KurtosisFlagBiasCorrection {};

struct KurtosisFlagNoBiasCorrection {};

template <class KURTOSIS_FLAG>
struct KurtosisOperation {
	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input,
	                              idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT_TYPE, STATE, OP>(state, input, unary_input);
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input) {
		if (state.n == 0) {
			state.shift = static_cast<double>(input);
		}
		state.n++;
		const double value = static_cast<double>(input) - state.shift;
		state.sum += value;
		const double square = value * value;
		state.sum_sqr += square;
		state.sum_cub += square * value;
		state.sum_four += square * square;
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		if (source.n == 0) {
			return;
		}
		if (target.n == 0) {
			target = source;
			return;
		}
		const double n = static_cast<double>(source.n);
		const double d = source.shift - target.shift;
		const double d2 = d * d;
		target.n += source.n;
		target.sum_four +=
		    source.sum_four + 4 * d * source.sum_cub + 6 * d2 * source.sum_sqr + 4 * d2 * d * source.sum + n * d2 * d2;
		target.sum_cub += source.sum_cub + 3 * d * source.sum_sqr + 3 * d2 * source.sum + n * d2 * d;
		target.sum_sqr += source.sum_sqr + 2 * d * source.sum + n * d2;
		target.sum += source.sum + n * d;
	}

	template <class TARGET_TYPE, class STATE>
	static void Finalize(STATE &state, TARGET_TYPE &target, AggregateFinalizeData &finalize_data) {
		auto n = (double)state.n;
		if (n <= 1) {
			finalize_data.ReturnNull();
			return;
		}
		if (std::is_same<KURTOSIS_FLAG, KurtosisFlagBiasCorrection>::value && n <= 3) {
			finalize_data.ReturnNull();
			return;
		}
		double temp = 1 / n;
		//! This is necessary due to linux 32 bits
		long double temp_aux = 1 / n;
		if (state.sum_sqr - state.sum * state.sum * temp == 0 ||
		    state.sum_sqr - state.sum * state.sum * temp_aux == 0) {
			finalize_data.ReturnNull();
			return;
		}
		double m4 =
		    temp * (state.sum_four - 4 * state.sum_cub * state.sum * temp +
		            6 * state.sum_sqr * state.sum * state.sum * temp * temp - 3 * pow(state.sum, 4) * pow(temp, 3));

		double m2 = temp * (state.sum_sqr - state.sum * state.sum * temp);
		if (m2 <= 0) { // m2 shouldn't be below 0 but floating points are weird
			finalize_data.ReturnNull();
			return;
		}
		if (std::is_same<KURTOSIS_FLAG, KurtosisFlagNoBiasCorrection>::value) {
			target = m4 / (m2 * m2) - 3;
		} else {
			target = (n - 1) * ((n + 1) * m4 / (m2 * m2) - 3 * (n - 1)) / ((n - 2) * (n - 3));
		}
		if (!Value::DoubleIsFinite(target)) {
			throw OutOfRangeException("Kurtosis is out of range!");
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

} // namespace

AggregateFunction KurtosisFun::GetFunction() {
	auto result =
	    AggregateFunction::UnaryAggregate<KurtosisState, double, double, KurtosisOperation<KurtosisFlagBiasCorrection>>(
	        LogicalType::DOUBLE, LogicalType::DOUBLE);
	result.GetSignature().GetParameter(0).SetName("x");
	result.SetFallible();
	return result;
}

AggregateFunction KurtosisPopFun::GetFunction() {
	auto result = AggregateFunction::UnaryAggregate<KurtosisState, double, double,
	                                                KurtosisOperation<KurtosisFlagNoBiasCorrection>>(
	    LogicalType::DOUBLE, LogicalType::DOUBLE);
	result.GetSignature().GetParameter(0).SetName("x");
	result.SetFallible();
	return result;
}

} // namespace duckdb
