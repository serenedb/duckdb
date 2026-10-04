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
	AlterJobInfo(AlterJobType alter_job_type_p, AlterEntryData data)
	    : AlterInfo(AlterType::ALTER_JOB, std::move(data.qualified_name), data.if_not_found),
	      alter_job_type(alter_job_type_p) {
	}
	~AlterJobInfo() override = default;

	AlterJobType alter_job_type;
	JobSchedule schedule;
	unique_ptr<ParsedExpression> interval_expr;
	unique_ptr<ParsedExpression> offset_expr;

public:
	CatalogType GetCatalogType() const override {
		return CatalogType::JOB_ENTRY;
	}
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterInfo> Deserialize(Deserializer &deserializer);

private:
	AlterJobInfo() : AlterInfo(AlterType::ALTER_JOB), alter_job_type(AlterJobType::SUSPEND) {
	}
};

} // namespace duckdb
