#include "core_functions/aggregate/distributive_functions.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/common/algorithm.hpp"

namespace duckdb {

namespace {

struct SkewState {
	static constexpr const char *STATE_NAMES[] = {"n", "shift", "sum", "sum_sqr", "sum_cub"};
	using STATE_TYPE = StructStateType<idx_t, double, double, double, double>;

	idx_t n;
	double shift;
	double sum;
	double sum_sqr;
	double sum_cub;
};

struct SkewnessOperation {
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
		target.sum_cub += source.sum_cub + 3 * d * source.sum_sqr + 3 * d2 * source.sum + n * d2 * d;
		target.sum_sqr += source.sum_sqr + 2 * d * source.sum + n * d2;
		target.sum += source.sum + n * d;
	}

	template <class TARGET_TYPE, class STATE>
	static void Finalize(STATE &state, TARGET_TYPE &target, AggregateFinalizeData &finalize_data) {
		if (state.n <= 2) {
			finalize_data.ReturnNull();
			return;
		}
		double n = state.n;
		double temp = 1 / n;
		double raw_m2 = state.sum_sqr - state.sum * state.sum * temp;
		if (raw_m2 == 0) {
			finalize_data.ReturnNull();
			return;
		}
		double variance = temp * raw_m2;
		if (variance <= 0) {
			finalize_data.ReturnNull();
			return;
		}
		auto p = std::pow(variance, 3);
		double div = std::sqrt(p);
		double temp1 = std::sqrt(n * (n - 1)) / (n - 2);
		target = temp1 * temp *
		         (state.sum_cub - 3 * state.sum_sqr * state.sum * temp + 2 * pow(state.sum, 3) * temp * temp) / div;
		if (!Value::DoubleIsFinite(target)) {
			throw OutOfRangeException("SKEW is out of range!");
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

} // namespace

AggregateFunction SkewnessFun::GetFunction() {
	auto fun = AggregateFunction::UnaryAggregate<SkewState, double, double, SkewnessOperation>(LogicalType::DOUBLE,
	                                                                                           LogicalType::DOUBLE);
	fun.GetSignature().GetParameter(0).SetName("x");
	return fun;
}

} // namespace duckdb
