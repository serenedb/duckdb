#include "duckdb/parser/statement/load_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/main/extension_helper.hpp"

namespace duckdb {

BoundStatement Binder::Bind(LoadStatement &stmt) {
	const bool is_install =
	    stmt.info->load_type == LoadType::INSTALL || stmt.info->load_type == LoadType::FORCE_INSTALL;
	ExtensionHelper::ThrowExtensionRuntimeUnsupported(stmt.info->filename, is_install);
}

} // namespace duckdb
