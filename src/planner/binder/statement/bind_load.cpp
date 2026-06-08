#include "duckdb/parser/statement/load_statement.hpp"
#include "duckdb/planner/binder.hpp"
#ifdef SDB_ENABLE_TEST_EXTENSION_LOAD
#include "duckdb/planner/operator/logical_simple.hpp"
#include "duckdb/main/extension_install_info.hpp"
#include <algorithm>
#endif

namespace duckdb {

BoundStatement Binder::Bind(LoadStatement &stmt) {
#ifdef SDB_ENABLE_TEST_EXTENSION_LOAD
	BoundStatement result;
	result.types = {LogicalType::BOOLEAN};
	result.names = {"Success"};

	if (!stmt.info->repository.empty() && stmt.info->repo_is_alias) {
		auto repository_url = ExtensionRepository::TryGetRepositoryUrl(stmt.info->repository);
		if (repository_url.empty()) {
			throw BinderException("'%s' is not a known repository name. Are you trying to query from a repository by "
			                      "path? Use single quotes: `FROM '%s'`",
			                      stmt.info->repository, stmt.info->repository);
		}
	}

	result.plan = make_uniq<LogicalSimple>(LogicalOperatorType::LOGICAL_LOAD, std::move(stmt.info));

	auto &properties = GetStatementProperties();
	properties.output_type = QueryResultOutputType::FORCE_MATERIALIZED;
	properties.return_type = StatementReturnType::NOTHING;
	return result;
#else
	const bool is_install =
	    stmt.info->load_type == LoadType::INSTALL || stmt.info->load_type == LoadType::FORCE_INSTALL;
	const string action = is_install ? "INSTALL" : "LOAD";
	const string verb = is_install ? "installed" : "loaded";
	throw NotImplementedException(
	    action + " is not supported by SereneDB: extensions are compiled into the server binary and cannot be " + verb +
	    " at runtime.\nIf you need the \"" + stmt.info->filename +
	    "\" extension, please open an issue at https://github.com/serenedb/serenedb/issues.");
#endif
}

} // namespace duckdb
