#include "duckdb/storage/index.hpp"
#include "duckdb/common/radix.hpp"

namespace duckdb {

Index::Index(const vector<column_t> &column_ids, TableIOManager &table_io_manager, AttachedDatabase &db)

    : column_ids(column_ids), table_io_manager(table_io_manager), db(db) {
	// create the column id set
	column_id_set.insert(column_ids.begin(), column_ids.end());
}

void Index::RemapColumnIds(const vector<column_t> &new_column_ids) {
	column_ids = new_column_ids;
	column_id_set.clear();
	column_id_set.insert(column_ids.begin(), column_ids.end());
}

} // namespace duckdb
