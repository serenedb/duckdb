#include "duckdb/parser/statement/prepare_statement.hpp"

#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/to_string.hpp"

namespace duckdb {

PrepareStatement::PrepareStatement() : SQLStatement(StatementType::PREPARE_STATEMENT), statement(nullptr), name("") {
}

PrepareStatement::PrepareStatement(const PrepareStatement &other)
    : SQLStatement(other), statement(other.statement->Copy()), name(other.name),
      parameter_type_hints(other.parameter_type_hints) {
}

unique_ptr<SQLStatement> PrepareStatement::Copy() const {
	return unique_ptr<PrepareStatement>(new PrepareStatement(*this));
}

string PrepareStatement::ToString() const {
	string result = "";
	result += "PREPARE";
	result += " ";
	result += SQLIdentifier(name);
	for (idx_t i = 1;; i++) {
		auto hint = parameter_type_hints.find(to_string(i));
		if (hint == parameter_type_hints.end()) {
			result += i == 1 ? "" : ")";
			break;
		}
		result += i == 1 ? "(" : ", ";
		result += hint->second.ToString();
	}
	result += " ";
	result += "AS";
	result += " ";
	result += statement->ToString();
	// NOTE: We expect SQLStatement->ToString() to always end in a ';' ^
	return result;
}

} // namespace duckdb
