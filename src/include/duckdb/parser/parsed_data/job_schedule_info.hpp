#pragma once
#include "duckdb/common/enums/job_schedule_kind.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {
struct JobScheduleInfo {
	JobScheduleKind kind;
	unique_ptr<ParsedExpression> interval;
	unique_ptr<ParsedExpression> offset;
	unique_ptr<ParsedExpression> randomize;
	bool concurrent = false;

	unique_ptr<JobScheduleInfo> Copy() const {
		auto result = make_uniq<JobScheduleInfo>();
		result->kind = kind;
		result->interval = interval ? interval->Copy() : nullptr;
		result->offset = offset ? offset->Copy() : nullptr;
		result->randomize = randomize ? randomize->Copy() : nullptr;
		result->concurrent = concurrent;
		return result;
	}
};
} // namespace duckdb
