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

RestartSequenceInfo::RestartSequenceInfo() : AlterSequenceInfo(AlterSequenceType::RESTART_SEQUENCE) {
}

RestartSequenceInfo::RestartSequenceInfo(const AlterEntryData &data, optional<int64_t> restart_with_p)
    : AlterSequenceInfo(AlterSequenceType::RESTART_SEQUENCE, data), restart_with(restart_with_p) {
}

unique_ptr<AlterInfo> RestartSequenceInfo::Copy() const {
	auto result = make_uniq<RestartSequenceInfo>(GetAlterEntryData(), restart_with);
	result->usage_count = usage_count;
	return std::move(result);
}

string RestartSequenceInfo::ToString() const {
	auto result =
	    "ALTER SEQUENCE " + GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA) + " RESTART";
	if (restart_with) {
		result += " WITH " + to_string(*restart_with);
	}
	return result + ";";
}

} // namespace duckdb
