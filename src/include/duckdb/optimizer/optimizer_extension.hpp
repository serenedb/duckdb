//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/optimizer_extension.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/optimizer_type.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/main/extension_callback_manager.hpp"

namespace duckdb {
struct DBConfig;
class Optimizer;
class ClientContext;

//! The OptimizerExtensionInfo holds static information relevant to the optimizer extension
struct OptimizerExtensionInfo {
	virtual ~OptimizerExtensionInfo() {
	}
};

struct OptimizerExtensionInput {
	ClientContext &context;
	Optimizer &optimizer;
	optional_ptr<OptimizerExtensionInfo> info;
};

typedef void (*optimize_function_t)(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);
typedef void (*pre_optimize_function_t)(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

class OptimizerExtension {
public:
	//! Runs as the After-hook of `anchor` (or after the whole pipeline if
	//! `anchor == INVALID`). Mutates the plan in place.
	optimize_function_t optimize_function = nullptr;
	//! Runs as the Before-hook of `anchor` (or before the whole pipeline if
	//! `anchor == INVALID`). Mutates the plan in place.
	pre_optimize_function_t pre_optimize_function = nullptr;

	//! When set (!= INVALID), pre/optimize_function fire around this named
	//! built-in pass instead of at the pipeline edges. Use this when the
	//! rewrite needs to compose with a specific pass -- e.g. fire after
	//! EXPRESSION_REWRITER has folded constant casts, or before
	//! UNUSED_COLUMNS so synthetic columns get pruned naturally.
	OptimizerType anchor = OptimizerType::INVALID;

	//! Additional optimizer info passed to the optimize functions
	shared_ptr<OptimizerExtensionInfo> optimizer_info;

	static void Register(DBConfig &config, OptimizerExtension extension);
	static ExtensionCallbackIteratorHelper<OptimizerExtension> Iterate(ClientContext &context) {
		return ExtensionCallbackManager::Get(context).OptimizerExtensions();
	}
};

} // namespace duckdb
