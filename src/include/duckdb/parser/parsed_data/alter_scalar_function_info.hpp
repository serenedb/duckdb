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
enum class AlterScalarFunctionType : uint8_t { INVALID = 0, ADD_FUNCTION_OVERLOADS = 1, RENAME_SCALAR_FUNCTION = 2 };

struct AlterScalarFunctionInfo : public AlterInfo {
	AlterScalarFunctionInfo(AlterScalarFunctionType type, const AlterEntryData &data);
	~AlterScalarFunctionInfo() override;

	AlterScalarFunctionType alter_scalar_function_type;

public:
	CatalogType GetCatalogType() const override;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterInfo> Deserialize(Deserializer &deserializer);
};

//===--------------------------------------------------------------------===//
// RenameScalarFunctionInfo
//===--------------------------------------------------------------------===//
// Used for ALTER FUNCTION ... RENAME TO ... Note: in SereneDB user-defined
// functions may be stored as either scalar or table macros; the binder/catalog
// skip the usual entry-type lookup for this info so the schema handler can
// resolve either kind by name.
struct RenameScalarFunctionInfo : public AlterScalarFunctionInfo {
	RenameScalarFunctionInfo(const AlterEntryData &data, Identifier new_name);
	~RenameScalarFunctionInfo() override;

	Identifier new_name;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterScalarFunctionInfo> Deserialize(Deserializer &deserializer);

	bool DependentCanRebind() const override {
		return true;
	}

private:
	RenameScalarFunctionInfo();
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
