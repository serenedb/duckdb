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

#include "duckdb/common/sorting/row_sort.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "ska_sort.hpp"
#include "vergesort.h"

namespace duckdb {

namespace {

template <class SORT_KEY>
struct RowSortExtractKey {
	using result_type = uint64_t;

	const result_type &operator()(const SORT_KEY &key) const {
		return key.part0;
	}

	bool ByteIsSkippable(const idx_t &) const {
		return false;
	}

	bool Interrupted() const {
		return false;
	}

	bool requires_next_sort;
	idx_t ska_sort_width;
};

template <SortKeyType SORT_KEY_TYPE, class T>
void RowSortConstruct(const Vector &key, idx_t rows, idx_t first, data_ptr_t keys, ArenaAllocator &heap) {
	using SORT_KEY = SortKey<SORT_KEY_TYPE>;
	const auto values = FlatVector::GetData<T>(key);
	const auto sort_keys = reinterpret_cast<SORT_KEY *>(keys) + first;
	for (idx_t i = 0; i < rows; i++) {
		data_ptr_t heap_ptr = nullptr;
		if constexpr (!SORT_KEY::CONSTANT_SIZE) {
			if (values[i].GetSize() > SORT_KEY::INLINE_LENGTH) {
				heap_ptr = heap.Allocate(values[i].GetSize());
			}
		}
		sort_keys[i].Construct(values[i], heap_ptr);
		sort_keys[i].payload.u.pad = first + i;
	}
}

template <SortKeyType SORT_KEY_TYPE>
void RowSortKeys(data_ptr_t keys, idx_t count, idx_t sort_width, std::span<sel_t> order) {
	using SORT_KEY = SortKey<SORT_KEY_TYPE>;
	const auto begin = reinterpret_cast<SORT_KEY *>(keys);
	const auto end = begin + count;
	const RowSortExtractKey<SORT_KEY> extract_key {
	    .requires_next_sort = !SORT_KEY::CONSTANT_SIZE || SORT_KEY::INLINE_LENGTH != sizeof(uint64_t),
	    .ska_sort_width = MinValue<idx_t>(sort_width, sizeof(uint64_t)),
	};
	duckdb_vergesort::vergesort(begin, end, std::less<SORT_KEY>(),
	                            [&extract_key](SORT_KEY *fallback_begin, SORT_KEY *fallback_end) {
		                            duckdb_ska_sort::ska_sort(fallback_begin, fallback_end, extract_key);
	                            });
	for (idx_t i = 0; i < count; i++) {
		order[i] = UnsafeNumericCast<sel_t>(begin[i].payload.u.pad);
	}
}

} // namespace

RowSort::RowSort(const vector<LogicalType> &types, vector<OrderModifiers> modifiers_p, idx_t capacity_p)
    : modifiers(std::move(modifiers_p)), sort_width(0), capacity(capacity_p), count(0),
      heap(Allocator::DefaultAllocator()) {
	D_ASSERT(!types.empty() && types.size() == modifiers.size());
	for (const auto &type : types) {
		const auto physical_type = type.InternalType();
		if (!TypeIsConstantSize(physical_type)) {
			sort_width = DConstants::INVALID_INDEX;
			break;
		}
		sort_width += 1 + GetTypeIdSize(physical_type);
	}
	key_type = sort_width <= sizeof(int64_t) ? LogicalType::BIGINT : LogicalType::BLOB;
	idx_t key_size;
	if (sort_width <= SortKey<SortKeyType::PAYLOAD_FIXED_16>::INLINE_LENGTH) {
		sort_key_type = SortKeyType::PAYLOAD_FIXED_16;
		key_size = sizeof(SortKey<SortKeyType::PAYLOAD_FIXED_16>);
	} else if (sort_width <= SortKey<SortKeyType::PAYLOAD_FIXED_24>::INLINE_LENGTH) {
		sort_key_type = SortKeyType::PAYLOAD_FIXED_24;
		key_size = sizeof(SortKey<SortKeyType::PAYLOAD_FIXED_24>);
	} else if (sort_width <= SortKey<SortKeyType::PAYLOAD_FIXED_32>::INLINE_LENGTH) {
		sort_key_type = SortKeyType::PAYLOAD_FIXED_32;
		key_size = sizeof(SortKey<SortKeyType::PAYLOAD_FIXED_32>);
	} else {
		sort_key_type = SortKeyType::PAYLOAD_VARIABLE_32;
		key_size = sizeof(SortKey<SortKeyType::PAYLOAD_VARIABLE_32>);
	}
	keys = Allocator::DefaultAllocator().Allocate(capacity * key_size);
}

void RowSort::Append(DataChunk &chunk) {
	const auto rows = chunk.size();
	D_ASSERT(count + rows <= capacity);
	Vector key(key_type, rows);
	CreateSortKeyHelpers::CreateSortKey(chunk, modifiers, key);
	switch (sort_key_type) {
	case SortKeyType::PAYLOAD_FIXED_16:
		RowSortConstruct<SortKeyType::PAYLOAD_FIXED_16, int64_t>(key, rows, count, keys.get(), heap);
		break;
	case SortKeyType::PAYLOAD_FIXED_24:
		RowSortConstruct<SortKeyType::PAYLOAD_FIXED_24, string_t>(key, rows, count, keys.get(), heap);
		break;
	case SortKeyType::PAYLOAD_FIXED_32:
		RowSortConstruct<SortKeyType::PAYLOAD_FIXED_32, string_t>(key, rows, count, keys.get(), heap);
		break;
	case SortKeyType::PAYLOAD_VARIABLE_32:
		RowSortConstruct<SortKeyType::PAYLOAD_VARIABLE_32, string_t>(key, rows, count, keys.get(), heap);
		break;
	default:
		throw InternalException("RowSort::Append for %s", EnumUtil::ToString(sort_key_type));
	}
	count += rows;
}

void RowSort::Sort(std::span<sel_t> order) {
	D_ASSERT(order.size() == count);
	switch (sort_key_type) {
	case SortKeyType::PAYLOAD_FIXED_16:
		return RowSortKeys<SortKeyType::PAYLOAD_FIXED_16>(keys.get(), count, sort_width, order);
	case SortKeyType::PAYLOAD_FIXED_24:
		return RowSortKeys<SortKeyType::PAYLOAD_FIXED_24>(keys.get(), count, sort_width, order);
	case SortKeyType::PAYLOAD_FIXED_32:
		return RowSortKeys<SortKeyType::PAYLOAD_FIXED_32>(keys.get(), count, sort_width, order);
	case SortKeyType::PAYLOAD_VARIABLE_32:
		return RowSortKeys<SortKeyType::PAYLOAD_VARIABLE_32>(keys.get(), count, sort_width, order);
	default:
		throw InternalException("RowSort::Sort for %s", EnumUtil::ToString(sort_key_type));
	}
}

} // namespace duckdb
