#include "duckdb/parser/parsed_data/alter_sequence_info.hpp"

namespace duckdb {

AlterSequenceInfo::AlterSequenceInfo(AlterSequenceType type)
    : AlterInfo(AlterType::ALTER_SEQUENCE), alter_sequence_type(type) {
}

AlterSequenceInfo::AlterSequenceInfo(AlterSequenceType type, const AlterEntryData &data)
    : AlterInfo(AlterType::ALTER_SEQUENCE, data.GetQualifiedName(), data.if_not_found), alter_sequence_type(type) {
}

AlterSequenceInfo::~AlterSequenceInfo() {
}

CatalogType AlterSequenceInfo::GetCatalogType() const {
	return CatalogType::SEQUENCE_ENTRY;
}

} // namespace duckdb
