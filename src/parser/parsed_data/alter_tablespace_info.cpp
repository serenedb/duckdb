#include "duckdb/parser/parsed_data/alter_tablespace_info.hpp"

#include "duckdb/common/sql_identifier.hpp"

namespace duckdb {

unique_ptr<AlterInfo> AlterTablespaceInfo::Copy() const {
	return make_uniq_base<AlterInfo, AlterTablespaceInfo>(GetAlterEntryData(), new_name);
}

string AlterTablespaceInfo::ToString() const {
	return RenameEntryToString(GetCatalogType(), SQLIdentifier::ToString(GetQualifiedName().Name()), if_not_found,
	                           new_name);
}

} // namespace duckdb
