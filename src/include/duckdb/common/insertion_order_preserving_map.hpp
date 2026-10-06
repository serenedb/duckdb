//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/insertion_order_preserving_map.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/vector.hpp"

#include <absl/cleanup/cleanup.h>
#include <absl/container/flat_hash_set.h>

#include <stdexcept>

namespace duckdb {

template <typename KEY>
struct InsertionOrderPreservingMapKey;

template <>
struct InsertionOrderPreservingMapKey<string> {
	using hasher = CaseInsensitiveStringHashFunction;
	using key_equal = CaseInsensitiveStringEquality;
};

template <>
struct InsertionOrderPreservingMapKey<Identifier> {
	using hasher = IdentifierHashFunction;
	using key_equal = IdentifierEquality;
};

template <typename V, typename KEY = string, typename INDEX_MAP = case_insensitive_map_t<idx_t>>
class InsertionOrderPreservingMap {
public:
	using key_type = KEY;                                                               // NOLINT: match stl API
	using mapped_type = V;                                                              // NOLINT: match stl API
	using value_type = pair<KEY, V>;                                                    // NOLINT: match stl API
	using iterator = typename vector<value_type>::iterator;                             // NOLINT: match stl API
	using const_iterator = typename vector<value_type>::const_iterator;                 // NOLINT: match stl API
	using reverse_iterator = typename vector<value_type>::reverse_iterator;             // NOLINT: match stl API
	using const_reverse_iterator = typename vector<value_type>::const_reverse_iterator; // NOLINT: match stl API

public:
	iterator begin() { // NOLINT: match stl API
		return entries.begin();
	}
	iterator end() { // NOLINT: match stl API
		return entries.end();
	}
	const_iterator begin() const { // NOLINT: match stl API
		return entries.begin();
	}
	const_iterator end() const { // NOLINT: match stl API
		return entries.end();
	}
	reverse_iterator rbegin() { // NOLINT: match stl API
		return entries.rbegin();
	}
	reverse_iterator rend() { // NOLINT: match stl API
		return entries.rend();
	}
	const_reverse_iterator rbegin() const { // NOLINT: match stl API
		return entries.rbegin();
	}
	const_reverse_iterator rend() const { // NOLINT: match stl API
		return entries.rend();
	}

	size_t size() const { // NOLINT: match stl API
		return entries.size();
	}
	bool empty() const { // NOLINT: match stl API
		return entries.empty();
	}
	void reserve(size_t count) { // NOLINT: match stl API
		entries.reserve(count);
		index.reserve(count);
	}
	void clear() { // NOLINT: match stl API
		entries.clear();
		index.clear();
	}

	vector<KEY> Keys() const {
		vector<KEY> keys;
		keys.reserve(entries.size());
		for (auto &entry : entries) {
			keys.push_back(entry.first);
		}
		return keys;
	}

	template <class K>
	iterator find(const K &key) { // NOLINT: match stl API
		const auto slot = index.find(Lookup<K> {this, &key});
		return slot == index.end() ? entries.end() : entries.begin() + static_cast<ptrdiff_t>(slot->index);
	}
	template <class K>
	const_iterator find(const K &key) const { // NOLINT: match stl API
		const auto slot = index.find(Lookup<K> {this, &key});
		return slot == index.end() ? entries.end() : entries.begin() + static_cast<ptrdiff_t>(slot->index);
	}
	template <class K>
	bool contains(const K &key) const { // NOLINT: match stl API
		return index.contains(Lookup<K> {this, &key});
	}
	template <class K>
	V &at(const K &key) { // NOLINT: match stl API
		const auto entry = find(key);
		if (entry == end()) {
			throw std::out_of_range("InsertionOrderPreservingMap::at");
		}
		return entry->second;
	}
	template <class K>
	const V &at(const K &key) const { // NOLINT: match stl API
		const auto entry = find(key);
		if (entry == end()) {
			throw std::out_of_range("InsertionOrderPreservingMap::at");
		}
		return entry->second;
	}

	template <class K, class... ARGS>
	pair<iterator, bool> try_emplace(K &&key, ARGS &&... args) { // NOLINT: match stl API
		const Insert<std::remove_cvref_t<K>> probe {this, &key, hasher(key)};
		bool inserted = false;
		const auto slot = index.lazy_emplace(probe, [&](const auto &construct) {
			inserted = true;
			construct(Slot {probe.hash, entries.size()});
		});
		if (!inserted) {
			return {entries.begin() + static_cast<ptrdiff_t>(slot->index), false};
		}
		absl::Cleanup rollback = [&] {
			index.erase(slot);
		};
		entries.emplace_back(std::piecewise_construct, std::forward_as_tuple(std::forward<K>(key)),
		                     std::forward_as_tuple(std::forward<ARGS>(args)...));
		std::move(rollback).Cancel();
		return {std::prev(entries.end()), true};
	}
	template <class K, class M>
	pair<iterator, bool> insert(K &&key, M &&value) { // NOLINT: match stl API
		return try_emplace(std::forward<K>(key), std::forward<M>(value));
	}
	pair<iterator, bool> insert(value_type &&value) { // NOLINT: match stl API
		return try_emplace(std::move(value.first), std::move(value.second));
	}
	template <class K, class M>
	pair<iterator, bool> insert_or_assign(K &&key, M &&value) { // NOLINT: match stl API
		auto result = try_emplace(std::forward<K>(key), std::forward<M>(value));
		if (!result.second) {
			result.first->second = std::forward<M>(value);
		}
		return result;
	}
	template <class K>
	V &operator[](K &&key) {
		return try_emplace(std::forward<K>(key)).first->second;
	}

	void erase(const_iterator position) { // NOLINT: match stl API
		const auto offset = static_cast<size_t>(position - entries.cbegin());
		index.erase(index.find(Lookup<KEY> {this, &position->first}));
		for (auto &slot : index) {
			if (slot.index > offset) {
				slot.index--;
			}
		}
		entries.erase(position);
	}
	template <class K, typename std::enable_if<!std::is_convertible<const K &, const_iterator>::value, int>::type = 0>
	size_t erase(const K &key) { // NOLINT: match stl API
		const auto entry = find(key);
		if (entry == end()) {
			return 0;
		}
		erase(entry);
		return 1;
	}

	bool operator==(const InsertionOrderPreservingMap &other) const requires std::is_same_v<KEY, Identifier> {
		return entries == other.entries;
	}

private:
	using HASH = typename InsertionOrderPreservingMapKey<KEY>::hasher;
	using EQUAL = typename InsertionOrderPreservingMapKey<KEY>::key_equal;

	struct Slot {
		size_t hash;
		mutable size_t index;
	};

	template <class K>
	struct Lookup {
		const InsertionOrderPreservingMap *map;
		const K *key;
	};

	template <class K>
	struct Insert {
		const InsertionOrderPreservingMap *map;
		const K *key;
		size_t hash;
	};

	struct SlotHash {
		using is_transparent = void;

		size_t operator()(const Slot &slot) const {
			return slot.hash;
		}
		template <class K>
		size_t operator()(const Lookup<K> &probe) const {
			return probe.map->hasher(*probe.key);
		}
		template <class K>
		size_t operator()(const Insert<K> &probe) const {
			return probe.hash;
		}
	};

	struct SlotEqual {
		using is_transparent = void;

		bool operator()(const Slot &lhs, const Slot &rhs) const {
			return lhs.index == rhs.index;
		}
		template <class PROBE>
		bool operator()(const Slot &slot, const PROBE &probe) const {
			return probe.map->equal(probe.map->entries[slot.index].first, *probe.key);
		}
	};

	vector<value_type> entries;
	absl::flat_hash_set<Slot, SlotHash, SlotEqual> index;
	[[no_unique_address]] HASH hasher;
	[[no_unique_address]] EQUAL equal;
};

} // namespace duckdb
