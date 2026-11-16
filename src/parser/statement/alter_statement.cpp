#include "duckdb/parser/statement/alter_statement.hpp"

namespace duckdb {

AlterStatement::AlterStatement() : SQLStatement(StatementType::ALTER_STATEMENT) {
}

AlterStatement::AlterStatement(unique_ptr<AlterInfo> info) : AlterStatement() {
	infos.push_back(std::move(info));
}

AlterStatement::AlterStatement(const AlterStatement &other) : SQLStatement(other) {
	for (auto &info : other.infos) {
		infos.push_back(info->Copy());
	}
}

unique_ptr<SQLStatement> AlterStatement::Copy() const {
	return unique_ptr<AlterStatement>(new AlterStatement(*this));
}

string AlterStatement::ToString() const {
	return infos[0]->StatementToString(infos);
}

} // namespace duckdb
