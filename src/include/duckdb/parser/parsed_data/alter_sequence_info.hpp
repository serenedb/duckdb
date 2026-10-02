//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/alter_sequence_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"

namespace duckdb {

enum class AlterSequenceType : uint8_t { INVALID = 0, RENAME_SEQUENCE = 200, RESTART_SEQUENCE = 201 };

struct AlterSequenceInfo : public AlterInfo {
	AlterSequenceInfo(AlterSequenceType type, const AlterEntryData &data);
	~AlterSequenceInfo() override;

	AlterSequenceType alter_sequence_type;

public:
	CatalogType GetCatalogType() const override;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterInfo> Deserialize(Deserializer &deserializer);

protected:
	explicit AlterSequenceInfo(AlterSequenceType type);
};

struct RenameSequenceInfo
    : public RenameEntryInfo<RenameSequenceInfo, AlterSequenceInfo, AlterSequenceType::RENAME_SEQUENCE> {
	using RenameEntryInfo::RenameEntryInfo;

public:
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterSequenceInfo> Deserialize(Deserializer &deserializer);

private:
	RenameSequenceInfo() = default;
};

struct RestartSequenceInfo : public AlterSequenceInfo {
	RestartSequenceInfo(const AlterEntryData &data, optional<int64_t> restart_with);

	optional<int64_t> restart_with;
	uint64_t usage_count = 0;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterSequenceInfo> Deserialize(Deserializer &deserializer);

private:
	RestartSequenceInfo();
};

} // namespace duckdb
