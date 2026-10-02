//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/alter_job_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/job_schedule.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

struct AlterJobInfo : public AlterInfo {
	AlterJobInfo(AlterJobType alter_job_type, AlterEntryData data);
	~AlterJobInfo() override;

	AlterJobType alter_job_type;
	JobSchedule schedule;
	unique_ptr<ParsedExpression> interval_expr;
	unique_ptr<ParsedExpression> offset_expr;

public:
	CatalogType GetCatalogType() const override;
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterInfo> Deserialize(Deserializer &deserializer);

private:
	AlterJobInfo();
};

} // namespace duckdb
