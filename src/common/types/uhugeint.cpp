#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/windows_undefs.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/operator/cast_operators.hpp"

#include <cmath>
#include <limits>

namespace duckdb {

//===--------------------------------------------------------------------===//
// String Conversion
//===--------------------------------------------------------------------===//
const uhugeint_t Uhugeint::POWERS_OF_TEN[] {
    uhugeint_t(1),
    uhugeint_t(10),
    uhugeint_t(100),
    uhugeint_t(1000),
    uhugeint_t(10000),
    uhugeint_t(100000),
    uhugeint_t(1000000),
    uhugeint_t(10000000),
    uhugeint_t(100000000),
    uhugeint_t(1000000000),
    uhugeint_t(10000000000),
    uhugeint_t(100000000000),
    uhugeint_t(1000000000000),
    uhugeint_t(10000000000000),
    uhugeint_t(100000000000000),
    uhugeint_t(1000000000000000),
    uhugeint_t(10000000000000000),
    uhugeint_t(100000000000000000),
    uhugeint_t(1000000000000000000),
    uhugeint_t(1000000000000000000) * uhugeint_t(10),
    uhugeint_t(1000000000000000000) * uhugeint_t(100),
    uhugeint_t(1000000000000000000) * uhugeint_t(1000),
    uhugeint_t(1000000000000000000) * uhugeint_t(10000),
    uhugeint_t(1000000000000000000) * uhugeint_t(100000),
    uhugeint_t(1000000000000000000) * uhugeint_t(1000000),
    uhugeint_t(1000000000000000000) * uhugeint_t(10000000),
    uhugeint_t(1000000000000000000) * uhugeint_t(100000000),
    uhugeint_t(1000000000000000000) * uhugeint_t(1000000000),
    uhugeint_t(1000000000000000000) * uhugeint_t(10000000000),
    uhugeint_t(1000000000000000000) * uhugeint_t(100000000000),
    uhugeint_t(1000000000000000000) * uhugeint_t(1000000000000),
    uhugeint_t(1000000000000000000) * uhugeint_t(10000000000000),
    uhugeint_t(1000000000000000000) * uhugeint_t(100000000000000),
    uhugeint_t(1000000000000000000) * uhugeint_t(1000000000000000),
    uhugeint_t(1000000000000000000) * uhugeint_t(10000000000000000),
    uhugeint_t(1000000000000000000) * uhugeint_t(100000000000000000),
    uhugeint_t(1000000000000000000) * uhugeint_t(1000000000000000000),
    uhugeint_t(1000000000000000000) * uhugeint_t(1000000000000000000) * uhugeint_t(10),
    uhugeint_t(1000000000000000000) * uhugeint_t(1000000000000000000) * uhugeint_t(100)};

string Uhugeint::ToString(uhugeint_t input) {
	uhugeint_t remainder;
	string result;
	while (true) {
		if (!input.lower && !input.upper) {
			break;
		}
		input = Uhugeint::DivMod(input, 10, remainder);
		result = string(1, UnsafeNumericCast<char>('0' + remainder.lower)) + result; // NOLINT
	}
	if (result.empty()) {
		// value is zero
		return "0";
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Cast/Conversion
//===--------------------------------------------------------------------===//
template <>
bool Uhugeint::TryCast(uhugeint_t input, float &result) {
	double dbl_result;
	Uhugeint::TryCast(input, dbl_result);
	result = (float)dbl_result;
	return true;
}

template <class REAL_T>
bool CastUhugeintToFloating(uhugeint_t input, REAL_T &result) {
	result = REAL_T(input.lower) + REAL_T(input.upper) * REAL_T(NumericLimits<uint64_t>::Maximum());
	return true;
}

template <>
bool Uhugeint::TryCast(uhugeint_t input, double &result) {
	return CastUhugeintToFloating<double>(input, result);
}

template <>
bool Uhugeint::TryConvert(const char *value, uhugeint_t &result) {
	auto len = strlen(value);
	string_t string_val(value, UnsafeNumericCast<uint32_t>(len));
	return TryCast::Operation<string_t, uhugeint_t>(string_val, result, true);
}

template <>
bool Uhugeint::TryConvert(float value, uhugeint_t &result) {
	return Uhugeint::TryConvert(double(value), result);
}

template <class REAL_T>
bool ConvertFloatingToUhugeint(REAL_T value, uhugeint_t &result) {
	if (!Value::IsFinite<REAL_T>(value)) {
		return false;
	}
	if (value < 0 || value >= 340282366920938463463374607431768211456.0) {
		return false;
	}
	result.lower = (uint64_t)fmod(value, REAL_T(NumericLimits<uint64_t>::Maximum()));
	result.upper = (uint64_t)(value / REAL_T(NumericLimits<uint64_t>::Maximum()));
	return true;
}

template <>
bool Uhugeint::TryConvert(double value, uhugeint_t &result) {
	return ConvertFloatingToUhugeint<double>(value, result);
}

template <>
bool Uhugeint::TryConvert(long double value, uhugeint_t &result) {
	return ConvertFloatingToUhugeint<long double>(value, result);
}

//===--------------------------------------------------------------------===//
// uhugeint_t operators
//===--------------------------------------------------------------------===//
uhugeint_t::operator hugeint_t() const {
	return {static_cast<int64_t>(this->upper), this->lower};
}

string uhugeint_t::ToString() const {
	return Uhugeint::ToString(*this);
}

} // namespace duckdb
