#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"
#include "duckdb/common/operator/string_cast.hpp"
namespace duckdb {

BoundCastInfo DefaultCasts::DateCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		return BoundCastInfo(&VectorCastHelpers::StringCast<date_t, duckdb::StringCast>);
	case LogicalTypeId::TIMESTAMP:
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<date_t, timestamp_t, duckdb::TryCast>);
	case LogicalTypeId::TIMESTAMP_SEC:
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<date_t, timestamp_sec_t, duckdb::TryCast>);
	case LogicalTypeId::TIMESTAMP_MS:
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<date_t, timestamp_ms_t, duckdb::TryCast>);
	case LogicalTypeId::TIMESTAMP_NS:
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<date_t, timestamp_ns_t, duckdb::TryCast>);
	case LogicalTypeId::TIMESTAMP_TZ:
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<date_t, timestamp_t, duckdb::TryCast>);
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<date_t, timestamp_ns_t, duckdb::TryCast>);
	default:
		return TryVectorNullCast;
	}
}

BoundCastInfo DefaultCasts::TimeCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		return BoundCastInfo(&VectorCastHelpers::StringCast<dtime_t, duckdb::StringCast>);
	case LogicalTypeId::TIME_NS:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<dtime_t, dtime_ns_t, duckdb::Cast>);
	case LogicalTypeId::TIME_TZ:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<dtime_t, dtime_tz_t, duckdb::Cast>);
	case LogicalTypeId::INTERVAL:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<dtime_t, interval_t, duckdb::Cast>);
	default:
		return TryVectorNullCast;
	}
}

BoundCastInfo DefaultCasts::TimeNsCastSwitch(BindCastInput &input, const LogicalType &src, const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		return BoundCastInfo(&VectorCastHelpers::StringCast<dtime_ns_t, duckdb::StringCast>);
	case LogicalTypeId::TIME:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<dtime_ns_t, dtime_t, duckdb::Cast>);
	case LogicalTypeId::TIME_TZ:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<dtime_ns_t, dtime_tz_t, duckdb::Cast>);
	default:
		return TryVectorNullCast;
	}
}

BoundCastInfo DefaultCasts::TimeTzCastSwitch(BindCastInput &input, const LogicalType &source,
                                             const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		return BoundCastInfo(&VectorCastHelpers::StringCast<dtime_tz_t, duckdb::StringCast>);
	case LogicalTypeId::TIME:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<dtime_tz_t, dtime_t, duckdb::Cast>);
	case LogicalTypeId::TIME_NS:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<dtime_tz_t, dtime_ns_t, duckdb::Cast>);
	default:
		return TryVectorNullCast;
	}
}

BoundCastInfo DefaultCasts::TimestampCastSwitch(BindCastInput &input, const LogicalType &source,
                                                const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		return BoundCastInfo(&VectorCastHelpers::StringCast<timestamp_t, duckdb::StringCast>);
	case LogicalTypeId::DATE:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_t, date_t, duckdb::Cast>);
	case LogicalTypeId::TIME:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_t, dtime_t, duckdb::Cast>);
	case LogicalTypeId::TIME_NS:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_t, dtime_ns_t, duckdb::Cast>);
	case LogicalTypeId::TIME_TZ:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_t, dtime_tz_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP_SEC:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_t, timestamp_sec_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP_MS:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_t, timestamp_ms_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP_NS:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_t, timestamp_ns_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP_TZ:
		return ReinterpretCast;
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_t, timestamp_ns_t, duckdb::Cast>);
	default:
		return TryVectorNullCast;
	}
}

BoundCastInfo DefaultCasts::TimestampTzCastSwitch(BindCastInput &input, const LogicalType &source,
                                                  const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		return BoundCastInfo(&VectorCastHelpers::StringCast<timestamp_tz_t, duckdb::StringCast>);
	case LogicalTypeId::TIME_TZ:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_tz_t, dtime_tz_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP:
		return ReinterpretCast;
	case LogicalTypeId::TIMESTAMP_SEC:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_tz_t, timestamp_sec_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP_MS:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_tz_t, timestamp_ms_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP_NS:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_tz_t, timestamp_ns_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_tz_t, timestamp_tz_ns_t, duckdb::Cast>);
	default:
		return TryVectorNullCast;
	}
}

BoundCastInfo DefaultCasts::TimestampTzNsCastSwitch(BindCastInput &input, const LogicalType &source,
                                                    const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		return BoundCastInfo(&VectorCastHelpers::StringCast<timestamp_tz_ns_t, duckdb::StringCast>);
	case LogicalTypeId::TIME_TZ:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_tz_ns_t, dtime_tz_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP_NS:
		return ReinterpretCast;
	case LogicalTypeId::TIMESTAMP_TZ:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_tz_ns_t, timestamp_t, duckdb::Cast>);
	default:
		return TryVectorNullCast;
	}
}

BoundCastInfo DefaultCasts::TimestampNsCastSwitch(BindCastInput &input, const LogicalType &source,
                                                  const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		return BoundCastInfo(&VectorCastHelpers::StringCast<timestamp_ns_t, duckdb::StringCast>);
	case LogicalTypeId::DATE:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_ns_t, date_t, duckdb::Cast>);
	case LogicalTypeId::TIME:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_ns_t, dtime_t, duckdb::Cast>);
	case LogicalTypeId::TIME_NS:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_ns_t, dtime_ns_t, duckdb::Cast>);
	case LogicalTypeId::TIME_TZ:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_ns_t, dtime_tz_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_ns_t, timestamp_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP_SEC:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_ns_t, timestamp_sec_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP_MS:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_ns_t, timestamp_ms_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP_TZ:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_ns_t, timestamp_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return ReinterpretCast;
	default:
		return TryVectorNullCast;
	}
}

BoundCastInfo DefaultCasts::TimestampMsCastSwitch(BindCastInput &input, const LogicalType &source,
                                                  const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		return BoundCastInfo(&VectorCastHelpers::StringCast<timestamp_ms_t, duckdb::CastFromTimestampMS>);
	case LogicalTypeId::DATE:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_ms_t, date_t, duckdb::Cast>);
	case LogicalTypeId::TIME:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_ms_t, dtime_t, duckdb::Cast>);
	case LogicalTypeId::TIME_NS:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_ms_t, dtime_ns_t, duckdb::Cast>);
	case LogicalTypeId::TIME_TZ:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_ms_t, dtime_tz_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_ms_t, timestamp_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP_SEC:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_ms_t, timestamp_sec_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP_NS:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_ms_t, timestamp_ns_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP_TZ:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_ms_t, timestamp_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_ms_t, timestamp_ns_t, duckdb::Cast>);
	default:
		return TryVectorNullCast;
	}
}

BoundCastInfo DefaultCasts::TimestampSecCastSwitch(BindCastInput &input, const LogicalType &source,
                                                   const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		return BoundCastInfo(&VectorCastHelpers::StringCast<timestamp_sec_t, duckdb::CastFromTimestampSec>);
	case LogicalTypeId::DATE:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_sec_t, date_t, duckdb::Cast>);
	case LogicalTypeId::TIME:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_sec_t, dtime_t, duckdb::Cast>);
	case LogicalTypeId::TIME_NS:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_sec_t, dtime_ns_t, duckdb::Cast>);
	case LogicalTypeId::TIME_TZ:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_sec_t, dtime_tz_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_sec_t, timestamp_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP_MS:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_sec_t, timestamp_ms_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP_NS:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_sec_t, timestamp_ns_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP_TZ:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_sec_t, timestamp_t, duckdb::Cast>);
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<timestamp_sec_t, timestamp_ns_t, duckdb::Cast>);
	default:
		return TryVectorNullCast;
	}
}
BoundCastInfo DefaultCasts::IntervalCastSwitch(BindCastInput &input, const LogicalType &source,
                                               const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		return BoundCastInfo(&VectorCastHelpers::StringCast<interval_t, duckdb::StringCast>);
	case LogicalTypeId::TIME:
		return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<interval_t, dtime_t, duckdb::Cast>);
	default:
		return TryVectorNullCast;
	}
}

} // namespace duckdb
