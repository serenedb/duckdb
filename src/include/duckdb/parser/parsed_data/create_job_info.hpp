//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/create_job_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/catalog/job_schedule.hpp"
#include "duckdb/parser/parsed_data/create_info.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

struct CreateJobInfo : public CreateInfo {
	CreateJobInfo();

	const Identifier &GetJobName() const {
		return qualified_name.Name();
	}
	void SetJobName(Identifier name) {
		qualified_name = qualified_name.WithName(std::move(name));
	}

	JobSchedule schedule;
	bool suspended = false;
	string body;
	vector<CatalogSearchEntry> search_path;
	unique_ptr<ParsedExpression> interval_expr;
	unique_ptr<ParsedExpression> offset_expr;

public:
	DUCKDB_API void Serialize(Serializer &serializer) const override;
	DUCKDB_API static unique_ptr<CreateInfo> Deserialize(Deserializer &deserializer);

	unique_ptr<CreateInfo> Copy() const override;
	string ToString() const override;

	static string ScheduleToString(const JobSchedule &schedule, const unique_ptr<ParsedExpression> &interval_expr,
	                               const unique_ptr<ParsedExpression> &offset_expr);
};

} // namespace duckdb
