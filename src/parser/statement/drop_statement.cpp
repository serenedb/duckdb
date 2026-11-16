#include "duckdb/parser/statement/drop_statement.hpp"

namespace duckdb {

DropStatement::DropStatement() : SQLStatement(StatementType::DROP_STATEMENT) {
}

DropStatement::DropStatement(unique_ptr<DropInfo> info) : DropStatement() {
	infos.push_back(std::move(info));
}

DropStatement::DropStatement(const DropStatement &other) : SQLStatement(other) {
	for (auto &info : other.infos) {
		infos.push_back(info->Copy());
	}
}

unique_ptr<SQLStatement> DropStatement::Copy() const {
	return unique_ptr<DropStatement>(new DropStatement(*this));
}

string DropStatement::ToString() const {
	return DropInfo::ToString(infos);
}

} // namespace duckdb
