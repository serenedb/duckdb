#include "duckdb/parser/statement/load_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_simple.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"

namespace duckdb {

BoundStatement Binder::Bind(LoadStatement &stmt) {
	const bool is_install =
	    stmt.info->load_type == LoadType::INSTALL || stmt.info->load_type == LoadType::FORCE_INSTALL;
	const auto extension_name = ExtensionHelper::GetExtensionName(stmt.info->filename);

	// SereneDB compiles its extension set into the server binary. Asking for one
	// of those is accepted -- there is nothing to fetch, and LOAD still registers
	// it with this database -- so scripts carrying the usual DuckDB
	// `INSTALL x; LOAD x;` preamble work unchanged. Anything outside that set
	// genuinely cannot be provided at runtime.
	if (!ExtensionHelper::IsLinkedExtension(extension_name)) {
		ExtensionHelper::ThrowExtensionRuntimeUnsupported(extension_name, is_install);
	}

	BoundStatement result;
	result.types = {LogicalType::BOOLEAN};
	result.names = {"Success"};
	result.plan = make_uniq<LogicalSimple>(LogicalOperatorType::LOGICAL_LOAD, std::move(stmt.info));

	auto &properties = GetStatementProperties();
	properties.output_type = QueryResultOutputType::FORCE_MATERIALIZED;
	properties.return_type = StatementReturnType::NOTHING;
	return result;
}

} // namespace duckdb
