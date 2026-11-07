#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/create_sort_key.hpp"

namespace duckdb {

Vector &MapVector::GetKeys(Vector &vector) {
	auto &entries = StructVector::GetEntries(ListVector::GetChildMutable(vector));
	D_ASSERT(entries.size() == 2);
	return entries[0];
}
Vector &MapVector::GetValues(Vector &vector) {
	auto &entries = StructVector::GetEntries(ListVector::GetChildMutable(vector));
	D_ASSERT(entries.size() == 2);
	return entries[1];
}

const Vector &MapVector::GetKeys(const Vector &vector) {
	return GetKeys((Vector &)vector);
}
const Vector &MapVector::GetValues(const Vector &vector) {
	return GetValues((Vector &)vector);
}

static void NormalizeIntervals(Vector &vector) {
	switch (vector.GetType().InternalType()) {
	case PhysicalType::INTERVAL: {
		Vector normalized(vector.GetType(), vector.size());
		UnaryExecutor::Execute<interval_t, interval_t>(vector, normalized, vector.size(),
		                                               [](interval_t input) { return input.Normalize(); });
		vector.Reference(normalized);
		break;
	}
	case PhysicalType::STRUCT:
		for (auto &child : StructVector::GetEntries(vector)) {
			NormalizeIntervals(child);
		}
		break;
	case PhysicalType::LIST:
		NormalizeIntervals(ListVector::GetChildMutable(vector));
		break;
	case PhysicalType::ARRAY:
		NormalizeIntervals(ArrayVector::GetChildMutable(vector));
		break;
	default:
		break;
	}
}

void MapVector::CreateKeySortKeys(const Vector &keys, Vector &result) {
	const OrderModifiers modifiers(OrderType::ASCENDING, OrderByNullType::NULLS_LAST);
	if (!TypeVisitor::Contains(keys.GetType(), LogicalTypeId::INTERVAL)) {
		CreateSortKeyHelpers::CreateSortKey(keys, modifiers, result);
		return;
	}
	Vector normalized(keys.GetType(), keys.size());
	normalized.Append(keys, keys.size(), VectorAppendMode::ALLOW_RESIZE);
	NormalizeIntervals(normalized);
	CreateSortKeyHelpers::CreateSortKey(normalized, modifiers, result);
}

MapValidityChecker::MapValidityChecker(const Vector &lists, const Vector &keys)
    : entries(lists.Values<list_entry_t>()), keys(keys), key_validity(keys.Validity()) {
}

MapInvalidReason MapValidityChecker::CheckRow(idx_t row) {
	auto entry = entries[row];
	if (!entry.IsValid()) {
		return MapInvalidReason::VALID;
	}
	auto &list = entry.GetValue();
	const bool check_duplicates = list.length > 1;
	if (check_duplicates) {
		if (!sort_keys) {
			sort_key_data = make_uniq<Vector>(LogicalType::BLOB, keys.size());
			MapVector::CreateKeySortKeys(keys, *sort_key_data);
			sort_keys = make_uniq<VectorIterator<string_t>>(*sort_key_data);
		}
		seen.clear();
	}
	for (idx_t i = 0; i < list.length; i++) {
		const auto key = list.offset + i;
		if (!key_validity.IsValid(key)) {
			return MapInvalidReason::NULL_KEY;
		}
		if (check_duplicates && !seen.insert((*sort_keys)[key].GetValue()).second) {
			return MapInvalidReason::DUPLICATE_KEY;
		}
	}
	return MapInvalidReason::VALID;
}

MapInvalidReason MapVector::CheckMapValidity(const Vector &map, idx_t count, const SelectionVector &sel) {
	D_ASSERT(map.GetType().id() == LogicalTypeId::MAP);
	if (map.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		count = MinValue<idx_t>(count, 1);
	}
	MapValidityChecker checker(map, MapVector::GetKeys(map));
	for (idx_t row_idx = 0; row_idx < count; row_idx++) {
		auto reason = checker.CheckRow(sel.get_index(row_idx));
		if (reason != MapInvalidReason::VALID) {
			return reason;
		}
	}
	return MapInvalidReason::VALID;
}

void MapVector::MapConversionVerify(const Vector &vector, idx_t count) {
	auto reason = MapVector::CheckMapValidity(vector, count);
	EvalMapInvalidReason(reason);
}

void MapVector::EvalMapInvalidReason(MapInvalidReason reason) {
	switch (reason) {
	case MapInvalidReason::VALID:
		return;
	case MapInvalidReason::DUPLICATE_KEY:
		throw InvalidInputException("Map keys must be unique.");
	case MapInvalidReason::NULL_KEY:
		throw InvalidInputException("Map keys can not be NULL.");
	case MapInvalidReason::NOT_ALIGNED:
		throw InvalidInputException("The map key list does not align with the map value list.");
	case MapInvalidReason::INVALID_PARAMS:
		throw InvalidInputException("Invalid map argument(s). Valid map arguments are a list of key-value pairs (MAP "
		                            "{'key1': 'val1', ...}), two lists (MAP ([1, 2], [10, 11])), or no arguments.");
	default:
		throw InternalException("MapInvalidReason not implemented");
	}
}

} // namespace duckdb
