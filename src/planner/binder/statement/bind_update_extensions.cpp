#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/statement/update_extensions_statement.hpp"
#ifdef SDB_ENABLE_TEST_EXTENSION_LOAD
#include "duckdb/parser/statement/load_statement.hpp"
#include "duckdb/planner/operator/logical_simple.hpp"
#include <algorithm>
#endif

namespace duckdb {

#ifdef SDB_ENABLE_TEST_EXTENSION_LOAD
BoundStatement Binder::Bind(UpdateExtensionsStatement &stmt) {
	BoundStatement result;

	result.names.emplace_back("extension_name");
	result.types.emplace_back(LogicalType::VARCHAR);
	result.names.emplace_back("repository");
	result.types.emplace_back(LogicalType::VARCHAR);
	result.names.emplace_back("update_result");
	result.types.emplace_back(LogicalType::VARCHAR);
	result.names.emplace_back("previous_version");
	result.types.emplace_back(LogicalType::VARCHAR);
	result.names.emplace_back("current_version");
	result.types.emplace_back(LogicalType::VARCHAR);

	result.plan = make_uniq<LogicalSimple>(LogicalOperatorType::LOGICAL_UPDATE_EXTENSIONS, std::move(stmt.info));

	return result;
}
#else
BoundStatement Binder::Bind(UpdateExtensionsStatement &) {
	throw NotImplementedException(
	    "UPDATE EXTENSIONS is not supported by SereneDB: extensions are compiled into the server binary and cannot "
	    "be updated at runtime.\n"
	    "If you are missing an extension, please open an issue at https://github.com/serenedb/serenedb/issues.");
}
#endif

} // namespace duckdb
