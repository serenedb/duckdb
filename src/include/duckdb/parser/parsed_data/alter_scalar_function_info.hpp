//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/alter_scalar_function_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/alter_info.hpp"

namespace duckdb {
struct CreateScalarFunctionInfo;

//===--------------------------------------------------------------------===//
// Alter Scalar Function
//===--------------------------------------------------------------------===//
enum class AlterScalarFunctionType : uint8_t { INVALID = 0, ADD_FUNCTION_OVERLOADS = 1, RENAME_SCALAR_FUNCTION = 200 };

struct AlterScalarFunctionInfo : public AlterInfo {
	AlterScalarFunctionInfo(AlterScalarFunctionType type, const AlterEntryData &data);
	~AlterScalarFunctionInfo() override;

	AlterScalarFunctionType alter_scalar_function_type;

public:
	CatalogType GetCatalogType() const override;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterInfo> Deserialize(Deserializer &deserializer);

protected:
	explicit AlterScalarFunctionInfo(AlterScalarFunctionType type);
};

//===--------------------------------------------------------------------===//
// RenameScalarFunctionInfo
//===--------------------------------------------------------------------===//
// Used for ALTER FUNCTION ... RENAME TO ... Note: in SereneDB user-defined
// functions may be stored as either scalar or table macros; the binder/catalog
// skip the usual entry-type lookup for this info so the schema handler can
// resolve either kind by name.
struct RenameScalarFunctionInfo : public RenameEntryInfo<RenameScalarFunctionInfo, AlterScalarFunctionInfo,
                                                         AlterScalarFunctionType::RENAME_SCALAR_FUNCTION> {
	using RenameEntryInfo::RenameEntryInfo;

public:
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterScalarFunctionInfo> Deserialize(Deserializer &deserializer);

private:
	RenameScalarFunctionInfo() = default;
};

//===--------------------------------------------------------------------===//
// AddScalarFunctionOverloadInfo
//===--------------------------------------------------------------------===//
struct AddScalarFunctionOverloadInfo : public AlterScalarFunctionInfo {
	AddScalarFunctionOverloadInfo(const AlterEntryData &data, unique_ptr<CreateScalarFunctionInfo> new_overloads);
	~AddScalarFunctionOverloadInfo() override;

	unique_ptr<CreateScalarFunctionInfo> new_overloads;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;
};

} // namespace duckdb
