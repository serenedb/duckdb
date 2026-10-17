#include "duckdb/common/time_point.hpp"

namespace duckdb {

static double TicksPerNano() {
	static const double ticks_per_nano = absl::base_internal::CycleClock::Frequency() / 1e9;
	return ticks_per_nano;
}

int64_t TimePoint::FromNanos(int64_t nanos) {
	return static_cast<int64_t>(static_cast<double>(nanos) * TicksPerNano());
}

int64_t TimePoint::ToNanos(int64_t ticks) {
	return static_cast<int64_t>(static_cast<double>(ticks) / TicksPerNano());
}

TimePoint::TimePoint() : ticks(0) {
}

TimePoint TimePoint::Tick() {
	return TimePoint(Now());
}

int64_t TimePoint::GetTickMs() {
	return ToNanos(Now()) / 1000000;
}

double TimePoint::ElapsedSeconds(const TimePoint &start, const TimePoint &end) {
	return static_cast<double>(ElapsedNanos(start, end)) / 1e9;
}

int64_t TimePoint::ElapsedMillis(const TimePoint &start, const TimePoint &end) {
	return ElapsedNanos(start, end) / 1000000;
}

int64_t TimePoint::ElapsedMicros(const TimePoint &start, const TimePoint &end) {
	return ElapsedNanos(start, end) / 1000;
}

int64_t TimePoint::ElapsedNanos(const TimePoint &start, const TimePoint &end) {
	return ToNanos(MaxValue<int64_t>(end.ticks - start.ticks, 0));
}

double TimePoint::ElapsedSeconds() const {
	return ElapsedSeconds(*this, Tick());
}

int64_t TimePoint::ElapsedMillis() const {
	return ElapsedMillis(*this, Tick());
}

int64_t TimePoint::ElapsedMicros() const {
	return ElapsedMicros(*this, Tick());
}

int64_t TimePoint::ElapsedNanos() const {
	return ElapsedNanos(*this, Tick());
}

TimePoint::TimePoint(int64_t ticks_p) : ticks(ticks_p) {
}

} // namespace duckdb
