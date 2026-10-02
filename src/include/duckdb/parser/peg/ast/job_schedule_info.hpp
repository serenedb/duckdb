#pragma once
#include "duckdb/common/enums/job_schedule_kind.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {
struct JobScheduleInfo {
	JobScheduleKind kind;
	unique_ptr<ParsedExpression> interval;
	unique_ptr<ParsedExpression> offset;
};
} // namespace duckdb
