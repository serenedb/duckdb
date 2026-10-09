//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/column_list.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/column_definition.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/optional_ptr.hpp"

#include <cstddef>
#include <iterator>

namespace duckdb {

//! A set of column definitions
class ColumnList {
public:
	class ColumnListIterator;

public:
	DUCKDB_API explicit ColumnList(bool allow_duplicate_names = false, bool case_sensitive = false);
	DUCKDB_API explicit ColumnList(vector<ColumnDefinition> columns, bool allow_duplicate_names = false,
	                               bool case_sensitive = false);

	DUCKDB_API void AddColumn(ColumnDefinition column);
	void Finalize();

	DUCKDB_API const ColumnDefinition &GetColumn(LogicalIndex index) const;
	DUCKDB_API const ColumnDefinition &GetColumn(PhysicalIndex index) const;
	DUCKDB_API const ColumnDefinition &GetColumn(const Identifier &name) const;
	DUCKDB_API ColumnDefinition &GetColumnMutable(LogicalIndex index);
	DUCKDB_API ColumnDefinition &GetColumnMutable(PhysicalIndex index);
	DUCKDB_API ColumnDefinition &GetColumnMutable(const Identifier &name);
	DUCKDB_API vector<string> GetColumnNames() const;
	DUCKDB_API vector<LogicalType> GetColumnTypes() const;

	DUCKDB_API bool ColumnExists(const Identifier &name) const;

	DUCKDB_API LogicalIndex GetColumnIndex(Identifier &column_name) const;
	DUCKDB_API PhysicalIndex LogicalToPhysical(LogicalIndex index) const;
	DUCKDB_API LogicalIndex PhysicalToLogical(PhysicalIndex index) const;

	idx_t LogicalColumnCount() const {
		return columns.size();
	}
	idx_t PhysicalColumnCount() const {
		return physical_columns.size();
	}
	bool empty() const { // NOLINT: match stl API
		return columns.empty();
	}

	ColumnList Copy() const;
	void Serialize(Serializer &serializer) const;
	static ColumnList Deserialize(Deserializer &deserializer);

	DUCKDB_API ColumnListIterator Logical() const;
	DUCKDB_API ColumnListIterator Physical() const;

	void SetAllowDuplicates(bool allow_duplicates) {
		allow_duplicate_names = allow_duplicates;
	}

	bool IsCaseSensitive() const {
		return case_sensitive;
	}
	DUCKDB_API void SetCaseSensitive(bool case_sensitive);

private:
	vector<ColumnDefinition> columns;
	//! A map of column name to column index
	identifier_map_t<column_t> name_map;
	//! The set of physical columns
	vector<idx_t> physical_columns;
	//! Allow duplicate names or not
	bool allow_duplicate_names;
	bool case_sensitive;

private:
	void AddToNameMap(ColumnDefinition &column);

public:
	// logical iterator
	class ColumnListIterator {
	public:
		ColumnListIterator(const ColumnList &list, bool physical) : list(&list), physical(physical) {
		}

	private:
		optional_ptr<const ColumnList> list;
		bool physical;

	private:
		class ColumnLogicalIteratorInternal {
		public:
			using iterator_category = std::forward_iterator_tag;
			using value_type = ColumnDefinition;
			using difference_type = std::ptrdiff_t;
			using pointer = const ColumnDefinition *;
			using reference = const ColumnDefinition &;

			ColumnLogicalIteratorInternal() = default;
			ColumnLogicalIteratorInternal(const ColumnList &list, bool physical, idx_t pos, idx_t end)
			    : list(&list), physical(physical), pos(pos), end(end) {
			}

			optional_ptr<const ColumnList> list;
			bool physical = false;
			idx_t pos = 0;
			idx_t end = 0;

		public:
			ColumnLogicalIteratorInternal &operator++() {
				pos++;
				return *this;
			}
			ColumnLogicalIteratorInternal operator++(int) {
				auto result = *this;
				pos++;
				return result;
			}
			bool operator==(const ColumnLogicalIteratorInternal &other) const {
				return pos == other.pos && end == other.end && list.get() == other.list.get();
			}
			bool operator!=(const ColumnLogicalIteratorInternal &other) const {
				return !(*this == other);
			}
			const ColumnDefinition &operator*() const {
				if (physical) {
					return list->GetColumn(PhysicalIndex(pos));
				} else {
					return list->GetColumn(LogicalIndex(pos));
				}
			}
		};

	public:
		idx_t Size() const {
			return physical ? list->PhysicalColumnCount() : list->LogicalColumnCount();
		}

		ColumnLogicalIteratorInternal begin() const { // NOLINT: match stl API
			return ColumnLogicalIteratorInternal(*list, physical, 0, Size());
		}
		ColumnLogicalIteratorInternal end() const { // NOLINT: match stl API
			return ColumnLogicalIteratorInternal(*list, physical, Size(), Size());
		}
	};
};

} // namespace duckdb
