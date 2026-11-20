#include "duckdb/storage/statistics/numeric_stats.hpp"

#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

#include <limits>

namespace duckdb {

namespace {

bool BoundsOrderValues(const LogicalType &type) {
	return type.id() != LogicalTypeId::SQLNULL && type.id() != LogicalTypeId::TIME_TZ;
}

template <class T>
void InitializeEmptyFloatingPointStats(BaseStatistics &result) {
	NumericStats::SetMin(result, std::numeric_limits<T>::quiet_NaN());
	NumericStats::SetMax(result, NumericLimits<T>::Minimum());
}

template <class T>
bool TryCastBounds(const BaseStatistics &stats, T &min, T &max) {
	const auto &type = stats.GetType();
	if (!type.IsNumeric() || !NumericStats::HasMinMax(stats)) {
		return false;
	}
	return VisitPhysicalType(type.InternalType(), [&]<class S>() {
		if constexpr (std::is_same_v<S, interval_t>) {
			return false;
		} else {
			const auto lower = NumericStats::GetMinUnsafe<S>(stats);
			const auto upper = NumericStats::GetMaxUnsafe<S>(stats);
			if constexpr (std::is_same_v<S, int16_t> || std::is_same_v<S, int32_t> || std::is_same_v<S, int64_t> ||
			              std::is_same_v<S, hugeint_t>) {
				if (type.id() == LogicalTypeId::DECIMAL) {
					CastParameters parameters;
					const auto width = DecimalType::GetWidth(type);
					const auto scale = DecimalType::GetScale(type);
					return TryCastFromDecimal::Operation(lower, min, parameters, width, scale) &&
					       TryCastFromDecimal::Operation(upper, max, parameters, width, scale);
				}
			}
			return TryCast::Operation(lower, min) && TryCast::Operation(upper, max);
		}
	});
}

} // namespace

BaseStatistics NumericStats::CreateUnknown(LogicalType type) {
	BaseStatistics result(std::move(type));
	result.InitializeUnknown();
	SetMin(result, Value(result.GetType()));
	SetMax(result, Value(result.GetType()));
	return result;
}

BaseStatistics NumericStats::CreateEmpty(LogicalType type) {
	BaseStatistics result(std::move(type));
	result.InitializeEmpty();
	switch (result.GetType().InternalType()) {
	case PhysicalType::FLOAT:
		InitializeEmptyFloatingPointStats<float>(result);
		break;
	case PhysicalType::DOUBLE:
		InitializeEmptyFloatingPointStats<double>(result);
		break;
	default:
		SetMin(result, Value::MaximumValue(result.GetType()));
		SetMax(result, Value::MinimumValue(result.GetType()));
		break;
	}
	return result;
}

NumericStatsData &NumericStats::GetDataUnsafe(BaseStatistics &stats) {
	D_ASSERT(stats.GetStatsType() == StatisticsType::NUMERIC_STATS);
	return stats.stats_union.numeric_data;
}

const NumericStatsData &NumericStats::GetDataUnsafe(const BaseStatistics &stats) {
	D_ASSERT(stats.GetStatsType() == StatisticsType::NUMERIC_STATS);
	return stats.stats_union.numeric_data;
}

const NumericValueUnion &NumericStats::GetBoundUnsafe(const BaseStatistics &stats, bool min, PhysicalType type,
                                                      idx_t size) {
	auto &data = GetDataUnsafe(stats);
	D_ASSERT(min ? data.has_min : data.has_max);
	D_ASSERT(type == stats.GetType().InternalType() ||
	         (type == PhysicalType::INT8 && stats.GetType().InternalType() == PhysicalType::BOOL));
	D_ASSERT(size == GetTypeIdSize(type));
	return min ? data.min : data.max;
}

void NumericStats::Merge(BaseStatistics &stats, const BaseStatistics &other) {
	if (other.GetType().id() == LogicalTypeId::VALIDITY) {
		return;
	}
	D_ASSERT(stats.GetType() == other.GetType());
	auto &data = GetDataUnsafe(stats);
	const auto &other_data = GetDataUnsafe(other);
	VisitPhysicalType(stats.GetType().InternalType(), [&]<class T>() {
		// If the min and max of the stats are not valid, we need to merge the min and max of the other stats.
		if (data.has_min && data.has_max && GreaterThan::Operation(GetMinUnsafe<T>(stats), GetMaxUnsafe<T>(stats))) {
			data.has_min = other_data.has_min;
			data.has_max = other_data.has_max;
			data.min = other_data.min;
			data.max = other_data.max;
			return;
		}
		if (other_data.has_min && data.has_min) {
			if (LessThan::Operation(GetMinUnsafe<T>(other), GetMinUnsafe<T>(stats))) {
				data.min = other_data.min;
			}
		} else {
			data.has_min = false;
		}
		if (other_data.has_max && data.has_max) {
			if (GreaterThan::Operation(GetMaxUnsafe<T>(other), GetMaxUnsafe<T>(stats))) {
				data.max = other_data.max;
			}
		} else {
			data.has_max = false;
		}
	});
}

void NumericStats::ClampMin(BaseStatistics &stats, const BaseStatistics &other) {
	D_ASSERT(stats.GetType() == other.GetType() && HasMin(stats) && HasMin(other));
	VisitPhysicalType(stats.GetType().InternalType(), [&]<class T>() {
		if (LessThan::Operation(GetMinUnsafe<T>(stats), GetMinUnsafe<T>(other))) {
			GetDataUnsafe(stats).min = GetDataUnsafe(other).min;
		}
	});
}

void NumericStats::ClampMax(BaseStatistics &stats, const BaseStatistics &other) {
	D_ASSERT(stats.GetType() == other.GetType() && HasMax(stats) && HasMax(other));
	VisitPhysicalType(stats.GetType().InternalType(), [&]<class T>() {
		if (GreaterThan::Operation(GetMaxUnsafe<T>(stats), GetMaxUnsafe<T>(other))) {
			GetDataUnsafe(stats).max = GetDataUnsafe(other).max;
		}
	});
}

template <class T>
FilterPropagateResult CheckZonemapTemplated(const BaseStatistics &stats, ExpressionType comparison_type,
                                            array_ptr<const Value> constants) {
	T min_value = NumericStats::GetMinUnsafe<T>(stats);
	T max_value = NumericStats::GetMaxUnsafe<T>(stats);
	for (auto &constant_value : constants) {
		D_ASSERT(constant_value.type() == stats.GetType());
		D_ASSERT(!constant_value.IsNull());
		T constant = constant_value.GetValueUnsafe<T>();
		auto prune_result = NumericStats::CheckRange<T>(comparison_type, min_value, max_value, constant);
		if (prune_result == FilterPropagateResult::NO_PRUNING_POSSIBLE) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		} else if (prune_result == FilterPropagateResult::FILTER_ALWAYS_TRUE) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
	}
	return FilterPropagateResult::FILTER_ALWAYS_FALSE;
}

FilterPropagateResult NumericStats::CheckZonemap(const BaseStatistics &stats, ExpressionType comparison_type,
                                                 array_ptr<const Value> constants) {
	if (!NumericStats::HasMinMax(stats)) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	D_ASSERT(stats.CanHaveNoNull());
	return VisitPhysicalType(stats.GetType().InternalType(),
	                         [&]<class T>() { return CheckZonemapTemplated<T>(stats, comparison_type, constants); });
}

bool NumericStats::ConstantsCoverRange(const BaseStatistics &stats, array_ptr<const Value> constants) {
	auto &type = stats.GetType();
	// values are compared as hugeint_t, which cannot hold large UINT128 values
	hugeint_t min_value;
	hugeint_t max_value;
	if (!type.IsIntegral() || type.InternalType() == PhysicalType::UINT128 ||
	    !TryCastRange(stats, min_value, max_value)) {
		return false;
	}
	// covering [min, max] requires at least max - min + 1 constants
	hugeint_t range;
	if (!TrySubtractOperator::Operation(max_value, min_value, range) ||
	    range >= NumericCast<int64_t>(constants.size())) {
		return false;
	}
	vector<hugeint_t> values;
	for (auto &constant_value : constants) {
		if (constant_value.type() != type) {
			return false;
		}
		auto constant = constant_value.GetValue<hugeint_t>();
		if (constant >= min_value && constant <= max_value) {
			values.push_back(constant);
		}
	}
	std::sort(values.begin(), values.end());
	if (values.empty() || values.front() != min_value || values.back() != max_value) {
		return false;
	}
	for (idx_t i = 1; i < values.size(); i++) {
		if (values[i - 1] != values[i] && values[i - 1] + 1 != values[i]) {
			return false;
		}
	}
	return true;
}

bool NumericStats::IsConstant(const BaseStatistics &stats) {
	const auto &data = GetDataUnsafe(stats);
	if (!data.has_min || !data.has_max) {
		throw InternalException("IsConstant() called on statistics that does not have min and max");
	}
	return VisitPhysicalType(stats.GetType().InternalType(), [&]<class T>() {
		const auto min = GetMinUnsafe<T>(stats);
		const auto max = GetMaxUnsafe<T>(stats);
		if (GreaterThan::Operation(max, min)) {
			return false;
		}
		if constexpr (std::is_floating_point_v<T>) {
			// -0.0 == 0.0, so a zero range is only constant if both bounds have the same sign
			return min != T(0) || std::signbit(min) == std::signbit(max);
		} else {
			return true;
		}
	});
}

void SetNumericValueInternal(const Value &input, const LogicalType &type, NumericValueUnion &val, bool &has_val) {
	if (input.IsNull()) {
		has_val = false;
		return;
	}
	if (input.type().InternalType() != type.InternalType()) {
		throw InternalException("SetMin or SetMax called with Value that does not match statistics' column value");
	}
	has_val = true;
	switch (type.InternalType()) {
	case PhysicalType::BOOL:
		val.value_.boolean = BooleanValue::Get(input);
		break;
	case PhysicalType::INT8:
		val.value_.tinyint = TinyIntValue::Get(input);
		break;
	case PhysicalType::INT16:
		val.value_.smallint = SmallIntValue::Get(input);
		break;
	case PhysicalType::INT32:
		val.value_.integer = IntegerValue::Get(input);
		break;
	case PhysicalType::INT64:
		val.value_.bigint = BigIntValue::Get(input);
		break;
	case PhysicalType::UINT8:
		val.value_.utinyint = UTinyIntValue::Get(input);
		break;
	case PhysicalType::UINT16:
		val.value_.usmallint = USmallIntValue::Get(input);
		break;
	case PhysicalType::UINT32:
		val.value_.uinteger = UIntegerValue::Get(input);
		break;
	case PhysicalType::UINT64:
		val.value_.ubigint = UBigIntValue::Get(input);
		break;
	case PhysicalType::INT128:
		val.value_.hugeint = HugeIntValue::Get(input);
		break;
	case PhysicalType::UINT128:
		val.value_.uhugeint = UhugeIntValue::Get(input);
		break;
	case PhysicalType::FLOAT:
		val.value_.float_ = FloatValue::Get(input);
		break;
	case PhysicalType::DOUBLE:
		val.value_.double_ = DoubleValue::Get(input);
		break;
	case PhysicalType::INTERVAL:
		val.value_.interval = IntervalValue::Get(input);
		break;
	default:
		throw InternalException("Unsupported type for NumericStatistics::SetValueInternal");
	}
}

void NumericStats::SetMin(BaseStatistics &stats, const Value &new_min) {
	auto &data = NumericStats::GetDataUnsafe(stats);
	SetNumericValueInternal(new_min, stats.GetType(), data.min, data.has_min);
}

void NumericStats::SetMax(BaseStatistics &stats, const Value &new_max) {
	auto &data = NumericStats::GetDataUnsafe(stats);
	SetNumericValueInternal(new_max, stats.GetType(), data.max, data.has_max);
}

Value NumericValueUnionToValueInternal(const LogicalType &type, const NumericValueUnion &val) {
	switch (type.InternalType()) {
	case PhysicalType::BOOL:
		return Value::BOOLEAN(val.value_.boolean);
	case PhysicalType::INT8:
		return Value::TINYINT(val.value_.tinyint);
	case PhysicalType::INT16:
		return Value::SMALLINT(val.value_.smallint);
	case PhysicalType::INT32:
		return Value::INTEGER(val.value_.integer);
	case PhysicalType::INT64:
		return Value::BIGINT(val.value_.bigint);
	case PhysicalType::UINT8:
		return Value::UTINYINT(val.value_.utinyint);
	case PhysicalType::UINT16:
		return Value::USMALLINT(val.value_.usmallint);
	case PhysicalType::UINT32:
		return Value::UINTEGER(val.value_.uinteger);
	case PhysicalType::UINT64:
		return Value::UBIGINT(val.value_.ubigint);
	case PhysicalType::INT128:
		return Value::HUGEINT(val.value_.hugeint);
	case PhysicalType::UINT128:
		return Value::UHUGEINT(val.value_.uhugeint);
	case PhysicalType::FLOAT:
		return Value::FLOAT(val.value_.float_);
	case PhysicalType::DOUBLE:
		return Value::DOUBLE(val.value_.double_);
	case PhysicalType::INTERVAL:
		return Value::INTERVAL(val.value_.interval);
	default:
		throw InternalException("Unsupported type for NumericValueUnionToValue");
	}
}

Value NumericValueUnionToValue(const LogicalType &type, const NumericValueUnion &val) {
	return NumericValueUnionToValueInternal(type, val).WithType(type);
}

bool NumericStats::HasMinMax(const BaseStatistics &stats) {
	return HasMin(stats) && HasMax(stats) && VisitPhysicalType(stats.GetType().InternalType(), [&]<class T>() {
		       return LessThanEquals::Operation(GetMinUnsafe<T>(stats), GetMaxUnsafe<T>(stats));
	       });
}

bool NumericStats::TryCastRange(const BaseStatistics &stats, double &min, double &max) {
	return TryCastBounds(stats, min, max);
}

bool NumericStats::TryCastRange(const BaseStatistics &stats, hugeint_t &min, hugeint_t &max) {
	return TryCastBounds(stats, min, max);
}

bool NumericStats::HasMin(const BaseStatistics &stats) {
	return BoundsOrderValues(stats.GetType()) && NumericStats::GetDataUnsafe(stats).has_min;
}

bool NumericStats::HasMax(const BaseStatistics &stats) {
	return BoundsOrderValues(stats.GetType()) && NumericStats::GetDataUnsafe(stats).has_max;
}

Value NumericStats::Min(const BaseStatistics &stats) {
	if (!NumericStats::HasMin(stats)) {
		throw InternalException("Min() called on statistics that does not have min");
	}
	return NumericValueUnionToValue(stats.GetType(), NumericStats::GetDataUnsafe(stats).min);
}

Value NumericStats::Max(const BaseStatistics &stats) {
	if (!NumericStats::HasMax(stats)) {
		throw InternalException("Max() called on statistics that does not have max");
	}
	return NumericValueUnionToValue(stats.GetType(), NumericStats::GetDataUnsafe(stats).max);
}

Value NumericStats::MinOrNull(const BaseStatistics &stats) {
	if (!NumericStats::HasMin(stats)) {
		return Value(stats.GetType());
	}
	return NumericStats::Min(stats);
}

Value NumericStats::MaxOrNull(const BaseStatistics &stats) {
	if (!NumericStats::HasMax(stats)) {
		return Value(stats.GetType());
	}
	return NumericStats::Max(stats);
}

static void SerializeNumericStatsValue(const LogicalType &type, NumericValueUnion val, bool has_value,
                                       Serializer &serializer) {
	serializer.WriteProperty(100, "has_value", has_value);
	if (!has_value) {
		return;
	}
	switch (type.InternalType()) {
	case PhysicalType::BOOL:
		serializer.WriteProperty(101, "value", val.value_.boolean);
		break;
	case PhysicalType::INT8:
		serializer.WriteProperty(101, "value", val.value_.tinyint);
		break;
	case PhysicalType::INT16:
		serializer.WriteProperty(101, "value", val.value_.smallint);
		break;
	case PhysicalType::INT32:
		serializer.WriteProperty(101, "value", val.value_.integer);
		break;
	case PhysicalType::INT64:
		serializer.WriteProperty(101, "value", val.value_.bigint);
		break;
	case PhysicalType::UINT8:
		serializer.WriteProperty(101, "value", val.value_.utinyint);
		break;
	case PhysicalType::UINT16:
		serializer.WriteProperty(101, "value", val.value_.usmallint);
		break;
	case PhysicalType::UINT32:
		serializer.WriteProperty(101, "value", val.value_.uinteger);
		break;
	case PhysicalType::UINT64:
		serializer.WriteProperty(101, "value", val.value_.ubigint);
		break;
	case PhysicalType::INT128:
		serializer.WriteProperty(101, "value", val.value_.hugeint);
		break;
	case PhysicalType::UINT128:
		serializer.WriteProperty(101, "value", val.value_.uhugeint);
		break;
	case PhysicalType::FLOAT:
		serializer.WriteProperty(101, "value", val.value_.float_);
		break;
	case PhysicalType::DOUBLE:
		serializer.WriteProperty(101, "value", val.value_.double_);
		break;
	case PhysicalType::INTERVAL:
		serializer.WriteProperty(101, "value", val.value_.interval);
		break;
	default:
		throw InternalException("Unsupported type for serializing numeric statistics");
	}
}

static void DeserializeNumericStatsValue(const LogicalType &type, NumericValueUnion &result, bool &has_stats,
                                         Deserializer &deserializer) {
	auto has_value = deserializer.ReadProperty<bool>(100, "has_value");
	if (!has_value) {
		has_stats = false;
		return;
	}
	has_stats = true;
	switch (type.InternalType()) {
	case PhysicalType::BOOL:
		result.value_.boolean = deserializer.ReadProperty<bool>(101, "value");
		break;
	case PhysicalType::INT8:
		result.value_.tinyint = deserializer.ReadProperty<int8_t>(101, "value");
		break;
	case PhysicalType::INT16:
		result.value_.smallint = deserializer.ReadProperty<int16_t>(101, "value");
		break;
	case PhysicalType::INT32:
		result.value_.integer = deserializer.ReadProperty<int32_t>(101, "value");
		break;
	case PhysicalType::INT64:
		result.value_.bigint = deserializer.ReadProperty<int64_t>(101, "value");
		break;
	case PhysicalType::UINT8:
		result.value_.utinyint = deserializer.ReadProperty<uint8_t>(101, "value");
		break;
	case PhysicalType::UINT16:
		result.value_.usmallint = deserializer.ReadProperty<uint16_t>(101, "value");
		break;
	case PhysicalType::UINT32:
		result.value_.uinteger = deserializer.ReadProperty<uint32_t>(101, "value");
		break;
	case PhysicalType::UINT64:
		result.value_.ubigint = deserializer.ReadProperty<uint64_t>(101, "value");
		break;
	case PhysicalType::INT128:
		result.value_.hugeint = deserializer.ReadProperty<hugeint_t>(101, "value");
		break;
	case PhysicalType::UINT128:
		result.value_.uhugeint = deserializer.ReadProperty<uhugeint_t>(101, "value");
		break;
	case PhysicalType::FLOAT:
		result.value_.float_ = deserializer.ReadProperty<float>(101, "value");
		break;
	case PhysicalType::DOUBLE:
		result.value_.double_ = deserializer.ReadProperty<double>(101, "value");
		break;
	case PhysicalType::INTERVAL:
		result.value_.interval = deserializer.ReadProperty<interval_t>(101, "value");
		break;
	default:
		throw InternalException("Unsupported type for serializing numeric statistics");
	}
}

void NumericStats::Serialize(const BaseStatistics &stats, Serializer &serializer) {
	auto &numeric_stats = NumericStats::GetDataUnsafe(stats);
	// INTERVAL stats are only serialized in V2.0.0 and later.
	if (stats.GetType().id() == LogicalTypeId::INTERVAL && !serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
		return;
	}
	serializer.WriteObject(200, "min", [&](Serializer &object) {
		SerializeNumericStatsValue(stats.GetType(), numeric_stats.min, numeric_stats.has_min, object);
	});
	serializer.WriteObject(201, "max", [&](Serializer &object) {
		SerializeNumericStatsValue(stats.GetType(), numeric_stats.max, numeric_stats.has_max, object);
	});
}

void NumericStats::Deserialize(Deserializer &deserializer, BaseStatistics &result) {
	auto &numeric_stats = NumericStats::GetDataUnsafe(result);
	if (!deserializer.CanDeserializeProperty(200, "min")) {
		return;
	}
	deserializer.ReadObject(200, "min", [&](Deserializer &object) {
		DeserializeNumericStatsValue(result.GetType(), numeric_stats.min, numeric_stats.has_min, object);
	});
	deserializer.ReadObject(201, "max", [&](Deserializer &object) {
		DeserializeNumericStatsValue(result.GetType(), numeric_stats.max, numeric_stats.has_max, object);
	});
}

child_list_t<Value> NumericStats::ToStruct(const BaseStatistics &stats) {
	child_list_t<Value> result;
	if (NumericStats::HasMinMax(stats)) {
		result.emplace_back("min", NumericStats::MinOrNull(stats));
		result.emplace_back("max", NumericStats::MaxOrNull(stats));
	}
	return result;
}

template <class T>
void NumericStats::TemplatedVerify(const BaseStatistics &stats, const Vector &vector, const SelectionVector &sel,
                                   idx_t count) {
	auto entries = vector.Values<T>();
	auto min_value = NumericStats::MinOrNull(stats);
	auto max_value = NumericStats::MaxOrNull(stats);
	for (idx_t i = 0; i < count; i++) {
		auto idx = sel.get_index(i);
		auto entry = entries[idx];
		if (!entry.IsValid()) {
			continue;
		}
		if (!min_value.IsNull() &&
		    LessThan::Operation(entry.GetValue(), min_value.GetValueUnsafe<T>())) { // LCOV_EXCL_START
			throw InternalException("Statistics mismatch: value is smaller than min.\nStatistics: %s\nVector: %s",
			                        stats.ToString(), vector.ToString());
		} // LCOV_EXCL_STOP
		if (!max_value.IsNull() && GreaterThan::Operation(entry.GetValue(), max_value.GetValueUnsafe<T>())) {
			throw InternalException("Statistics mismatch: value is bigger than max.\nStatistics: %s\nVector: %s",
			                        stats.ToString(), vector.ToString());
		}
	}
}

void NumericStats::Verify(const BaseStatistics &stats, const Vector &vector, const SelectionVector &sel, idx_t count) {
	auto &type = stats.GetType();
	switch (type.InternalType()) {
	case PhysicalType::BOOL:
		break;
	case PhysicalType::INT8:
		TemplatedVerify<int8_t>(stats, vector, sel, count);
		break;
	case PhysicalType::INT16:
		TemplatedVerify<int16_t>(stats, vector, sel, count);
		break;
	case PhysicalType::INT32:
		TemplatedVerify<int32_t>(stats, vector, sel, count);
		break;
	case PhysicalType::INT64:
		TemplatedVerify<int64_t>(stats, vector, sel, count);
		break;
	case PhysicalType::UINT8:
		TemplatedVerify<uint8_t>(stats, vector, sel, count);
		break;
	case PhysicalType::UINT16:
		TemplatedVerify<uint16_t>(stats, vector, sel, count);
		break;
	case PhysicalType::UINT32:
		TemplatedVerify<uint32_t>(stats, vector, sel, count);
		break;
	case PhysicalType::UINT64:
		TemplatedVerify<uint64_t>(stats, vector, sel, count);
		break;
	case PhysicalType::INT128:
		TemplatedVerify<hugeint_t>(stats, vector, sel, count);
		break;
	case PhysicalType::UINT128:
		TemplatedVerify<uhugeint_t>(stats, vector, sel, count);
		break;
	case PhysicalType::FLOAT:
		TemplatedVerify<float>(stats, vector, sel, count);
		break;
	case PhysicalType::DOUBLE:
		TemplatedVerify<double>(stats, vector, sel, count);
		break;
	case PhysicalType::INTERVAL:
		TemplatedVerify<interval_t>(stats, vector, sel, count);
		break;
	default:
		throw InternalException("Unsupported type %s for numeric statistics verify", type.ToString());
	}
}

} // namespace duckdb
