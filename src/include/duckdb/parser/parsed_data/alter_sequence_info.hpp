//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/alter_sequence_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/alter_info.hpp"

namespace duckdb {

enum class AlterSequenceType : uint8_t { INVALID = 0, RENAME_SEQUENCE = 200 };

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

} // namespace duckdb
