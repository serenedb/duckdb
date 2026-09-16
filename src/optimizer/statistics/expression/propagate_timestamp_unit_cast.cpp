////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2026 SereneDB GmbH, Berlin, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is SereneDB GmbH, Berlin, Germany
////////////////////////////////////////////////////////////////////////////////

#include "duckdb/optimizer/statistics_propagator.hpp"

namespace duckdb {

namespace {

bool IsNaiveTimestamp(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
		return true;
	default:
		return false;
	}
}

bool IsZonedTimestamp(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return true;
	default:
		return false;
	}
}

} // namespace

bool StatisticsPropagator::IsTimestampUnitCast(const LogicalType &source, const LogicalType &target) {
	return (IsNaiveTimestamp(source) && IsNaiveTimestamp(target)) ||
	       (IsZonedTimestamp(source) && IsZonedTimestamp(target));
}

} // namespace duckdb
