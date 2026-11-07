#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/function/cast/default_casts.hpp"

#include "duckdb/common/likely.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/integer_cast_operator.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/cast_helpers.hpp"

#include "duckdb/common/types/null_value.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

void CastParameters::InterruptCheck() const {
	if (context) {
		context->InterruptCheck();
	}
}

BindCastInfo::~BindCastInfo() {
}

BoundCastData::~BoundCastData() {
}

CastStatisticsInput::CastStatisticsInput(BoundCastInfo &bound_cast_p, const LogicalType &source_type_p,
                                         const LogicalType &target_type_p, const BaseStatistics &child_stats_p,
                                         optional_ptr<ClientContext> context_p)
    : source_type(source_type_p), target_type(target_type_p), child_stats(child_stats_p), context(context_p),
      bound_cast(bound_cast_p) {
}

void CastStatisticsInput::SetFunction(cast_function_t new_function) {
	bound_cast.function = new_function;
}

BoundCastInfo::BoundCastInfo(cast_function_t function_p, unique_ptr<BoundCastData> cast_data_p,
                             init_cast_local_state_t init_local_state_p)
    : function(function_p), init_local_state(init_local_state_p), cast_data(std::move(cast_data_p)) {
}

BoundCastInfo BoundCastInfo::Copy() const {
	auto result = BoundCastInfo(function, cast_data ? cast_data->Copy() : nullptr, init_local_state);
	result.statistics = statistics;
	result.bucket_rewrite = bucket_rewrite;
	return result;
}

unique_ptr<BaseStatistics> BoundCastInfo::PropagateStatistics(const LogicalType &source_type,
                                                              const LogicalType &target_type,
                                                              const BaseStatistics &child_stats,
                                                              optional_ptr<ClientContext> context) {
	if (!statistics) {
		return nullptr;
	}
	CastStatisticsInput input(*this, source_type, target_type, child_stats, context);
	return statistics(input);
}

bool DefaultCasts::NopCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	result.Reference(source);
	return true;
}

void HandleCastError::AssignError(const string &error_message, CastParameters &parameters,
                                  std::string_view error_subtype) {
	AssignError(error_message, parameters.error_message, parameters.cast_source, parameters.query_location,
	            error_subtype);
}

void HandleCastError::AssignError(const string &error_message, string *error_message_ptr,
                                  optional_ptr<const Expression> cast_source, QueryLocation error_location,
                                  std::string_view error_subtype) {
	string column;
	if (cast_source && cast_source->HasAlias()) {
		column = " when casting from source column " + cast_source->GetAlias();
	}
	if (!error_message_ptr) {
		if (error_subtype.empty()) {
			throw ConversionException(error_location, error_message + column);
		}
		throw ConversionException(Exception::InitializeExtraInfo(error_subtype, error_location),
		                          error_message + column);
	}
	if (error_message_ptr->empty()) {
		*error_message_ptr = error_message + column;
	}
}

namespace {

struct DecimalSyntaxOperation {
	template <class T, bool NEGATIVE>
	static bool HandleDigit(T &state, uint8_t digit) {
		return true;
	}

	template <class T, bool NEGATIVE>
	static bool HandleHexDigit(T &state, uint8_t digit) {
		return false;
	}

	template <class T, bool NEGATIVE>
	static bool HandleBinaryDigit(T &state, uint8_t digit) {
		return false;
	}

	template <class T, bool NEGATIVE>
	static bool HandleExponent(T &state, int32_t exponent) {
		return true;
	}

	template <class T, bool NEGATIVE, bool ALLOW_EXPONENT>
	static bool HandleDecimal(T &state, uint8_t digit) {
		return true;
	}

	template <class T, bool NEGATIVE>
	static bool Finalize(T &state) {
		return true;
	}
};

struct DecimalSyntaxData {};

template <char decimal_separator>
bool IsDecimalSyntax(string_t input) {
	DecimalSyntaxData syntax;
	return TryIntegerCast<DecimalSyntaxData, true, true, DecimalSyntaxOperation, false, decimal_separator>(
	    input.GetData(), input.GetSize(), syntax, false);
}

std::string_view StringToDecimalErrorSubtype(string_t input, char decimal_separator) {
	std::string_view text(input.GetData(), input.GetSize());
	while (!text.empty() && StringUtil::CharacterIsSpace(text.front())) {
		text.remove_prefix(1);
	}
	while (!text.empty() && StringUtil::CharacterIsSpace(text.back())) {
		text.remove_suffix(1);
	}
	if (StringUtil::CIEquals(text, "nan")) {
		return HandleCastError::FEATURE_NOT_SUPPORTED;
	}
	if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
		text.remove_prefix(1);
	}
	if (StringUtil::CIEquals(text, "infinity") || StringUtil::CIEquals(text, "inf")) {
		return HandleCastError::NUMERIC_VALUE_OUT_OF_RANGE;
	}
	if (decimal_separator == ',' ? IsDecimalSyntax<','>(input) : IsDecimalSyntax<'.'>(input)) {
		return HandleCastError::NUMERIC_VALUE_OUT_OF_RANGE;
	}
	return std::string_view();
}

} // namespace

void HandleCastError::AssignStringToDecimalError(string_t input, uint8_t width, uint8_t scale, char decimal_separator,
                                                 CastParameters &parameters) {
	string error = StringUtil::Format("Could not convert string \"%s\" to DECIMAL(%d,%d)", input.GetString(),
	                                  (int)width, (int)scale);
	AssignError(error, parameters,
	            parameters.error_message ? std::string_view() : StringToDecimalErrorSubtype(input, decimal_separator));
}

// NULL cast only works if all values in source are NULL, otherwise an unimplemented cast exception is thrown
bool DefaultCasts::TryVectorNullCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	bool success = true;
	if (VectorOperations::HasNotNull(source)) {
		HandleCastError::AssignError(TryCast::UnimplementedCastMessage(source.GetType(), result.GetType()), parameters);
		success = false;
	}
	ConstantVector::SetNull(result, count_t(count));
	return success;
}

bool DefaultCasts::ReinterpretCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	result.Reinterpret(source);
	return true;
}

bool BoundCastInfo::IsNopCast() const {
	return function == DefaultCasts::NopCast;
}

bool BoundCastInfo::IsNullCast() const {
	return function == DefaultCasts::TryVectorNullCast;
}

static bool NullTypeCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	// cast a NULL to another type, just copy the properties and change the type
	ConstantVector::SetNull(result, count_t(count));
	return true;
}

BoundCastInfo DefaultCasts::GetDefaultCastFunction(BindCastInput &input, const LogicalType &source,
                                                   const LogicalType &target) {
	D_ASSERT(source != target);

	if (target.id() == LogicalTypeId::VARIANT) {
		return ImplicitToVariantCast(input, source, target);
	}

	// then check if were casting to a union
	if (source.id() != LogicalTypeId::UNION && source.id() != LogicalTypeId::SQLNULL &&
	    source.id() != LogicalTypeId::VARIANT && target.id() == LogicalTypeId::UNION) {
		return ImplicitToUnionCast(input, source, target);
	}

	// else, switch on source type
	switch (source.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return NumericCastSwitch(input, source, target);
	case LogicalTypeId::POINTER:
		return PointerCastSwitch(input, source, target);
	case LogicalTypeId::UUID:
		return UUIDCastSwitch(input, source, target);
	case LogicalTypeId::DECIMAL:
		return DecimalCastSwitch(input, source, target);
	case LogicalTypeId::DATE:
		return DateCastSwitch(input, source, target);
	case LogicalTypeId::TIME:
		return TimeCastSwitch(input, source, target);
	case LogicalTypeId::TIME_NS:
		return TimeNsCastSwitch(input, source, target);
	case LogicalTypeId::TIME_TZ:
		return TimeTzCastSwitch(input, source, target);
	case LogicalTypeId::TIMESTAMP:
		return TimestampCastSwitch(input, source, target);
	case LogicalTypeId::TIMESTAMP_TZ:
		return TimestampTzCastSwitch(input, source, target);
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return TimestampTzNsCastSwitch(input, source, target);
	case LogicalTypeId::TIMESTAMP_NS:
		return TimestampNsCastSwitch(input, source, target);
	case LogicalTypeId::TIMESTAMP_MS:
		return TimestampMsCastSwitch(input, source, target);
	case LogicalTypeId::TIMESTAMP_SEC:
		return TimestampSecCastSwitch(input, source, target);
	case LogicalTypeId::INTERVAL:
		return IntervalCastSwitch(input, source, target);
	case LogicalTypeId::VARCHAR:
		return StringCastSwitch(input, source, target);
	case LogicalTypeId::BLOB:
		return BlobCastSwitch(input, source, target);
	case LogicalTypeId::BIT:
		return BitCastSwitch(input, source, target);
	case LogicalTypeId::SQLNULL:
		return NullTypeCast;
	case LogicalTypeId::MAP:
		return MapCastSwitch(input, source, target);
	case LogicalTypeId::STRUCT:
		return StructCastSwitch(input, source, target);
	case LogicalTypeId::TUPLE:
		return TupleCastSwitch(input, source, target);
	case LogicalTypeId::LIST:
		return ListCastSwitch(input, source, target);
	case LogicalTypeId::UNION:
		return UnionCastSwitch(input, source, target);
	case LogicalTypeId::VARIANT:
		return VariantCastSwitch(input, source, target);
	case LogicalTypeId::ENUM:
		return EnumCastSwitch(input, source, target);
	case LogicalTypeId::ARRAY:
		return ArrayCastSwitch(input, source, target);
	case LogicalTypeId::GEOMETRY:
		return GeoCastSwitch(input, source, target);
	case LogicalTypeId::TYPE:
		return TypeCastSwitch(input, source, target);
	case LogicalTypeId::BIGNUM:
		return BignumCastSwitch(input, source, target);
	default:
		return nullptr;
	}
}

} // namespace duckdb
