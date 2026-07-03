//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/statement_type.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/main/query_parameters.hpp"
#include "duckdb/common/enums/database_modification_type.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Statement Types
//===--------------------------------------------------------------------===//
enum class StatementType : uint8_t {
	INVALID_STATEMENT,      // invalid statement type
	SELECT_STATEMENT,       // select statement type
	INSERT_STATEMENT,       // insert statement type
	UPDATE_STATEMENT,       // update statement type
	CREATE_STATEMENT,       // create statement type
	DELETE_STATEMENT,       // delete statement type
	PREPARE_STATEMENT,      // prepare statement type
	EXECUTE_STATEMENT,      // execute statement type
	ALTER_STATEMENT,        // alter statement type
	TRANSACTION_STATEMENT,  // transaction statement type,
	COPY_STATEMENT,         // copy type
	ANALYZE_STATEMENT,      // analyze type
	VARIABLE_SET_STATEMENT, // variable set statement type
	CREATE_FUNC_STATEMENT,  // create func statement type
	EXPLAIN_STATEMENT,      // explain statement type
	DROP_STATEMENT,         // DROP statement type
	EXPORT_STATEMENT,       // EXPORT statement type
	PRAGMA_STATEMENT,       // PRAGMA statement type
	VACUUM_STATEMENT,       // VACUUM statement type
	CALL_STATEMENT,         // CALL statement type
	SET_STATEMENT,          // SET statement type
	LOAD_STATEMENT,         // LOAD statement type
	RELATION_STATEMENT,
	EXTENSION_STATEMENT,
	LOGICAL_PLAN_STATEMENT,
	ATTACH_STATEMENT,
	DETACH_STATEMENT,
	MULTI_STATEMENT,
	COPY_DATABASE_STATEMENT,
	UPDATE_EXTENSIONS_STATEMENT,
	MERGE_INTO_STATEMENT,
	CONNECT_STATEMENT,
	DISCONNECT_STATEMENT
};

DUCKDB_API std::string_view StatementTypeToString(StatementType type);

enum class StatementReturnType : uint8_t {
	QUERY_RESULT, // the statement returns a query result (e.g. for display to the user)
	CHANGED_ROWS, // the statement returns a single row containing the number of changed rows (e.g. an insert stmt)
	NOTHING       // the statement returns nothing
};

std::string_view StatementReturnTypeToString(StatementReturnType type);

class Catalog;
class CatalogEntry;
class ClientContext;

//! Verb bits the binder records per access, for the server's access-control
//! layer to enforce. Values mirror the server's privilege bitmask so it can use
//! them without a translation table; core treats them as an opaque bitmask.
enum class AccessVerb : uint8_t {
	NONE = 0,
	INSERT = 1 << 0,
	SELECT = 1 << 1,
	UPDATE = 1 << 2,
	DELETE = 1 << 3,
	TRUNCATE = 1 << 4,
};
inline AccessVerb operator|(AccessVerb a, AccessVerb b) {
	return static_cast<AccessVerb>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline AccessVerb &operator|=(AccessVerb &a, AccessVerb b) {
	a = a | b;
	return a;
}

//! One base-relation access discovered during binding, for the access-control
//! layer to enforce. `who` is the enclosing definer view (its owner is the
//! effective principal) or null for the caller. `read`/`write` are logical
//! (0-based, generated-PK-excluded) columns.
struct AccessRequirement {
	idx_t table_index = 0;
	const CatalogEntry *table = nullptr;
	const CatalogEntry *who = nullptr;
	AccessVerb verb = AccessVerb::NONE;
	unordered_set<idx_t> read;
	unordered_set<idx_t> write;
};

//! A struct containing various properties of a SQL statement
struct StatementProperties {
	StatementProperties()
	    : requires_valid_transaction(true), output_type(QueryResultOutputType::FORCE_MATERIALIZED),
	      bound_all_parameters(true), return_type(StatementReturnType::QUERY_RESULT), parameter_count(0),
	      always_require_rebind(false) {
	}

	struct CatalogIdentity {
		idx_t catalog_oid;
		optional_idx catalog_version;

		bool operator==(const CatalogIdentity &rhs) const {
			return catalog_oid == rhs.catalog_oid && catalog_version == rhs.catalog_version;
		}

		bool operator!=(const CatalogIdentity &rhs) const {
			return !operator==(rhs);
		}
	};

	struct ModificationInfo {
		CatalogIdentity identity;
		DatabaseModificationType modifications;
	};

	//! The set of databases this statement will read from
	identifier_map_t<CatalogIdentity> read_databases;
	//! The set of databases this statement will modify
	identifier_map_t<ModificationInfo> modified_databases;
	//! Every base-relation access the binder discovered, with the columns touched,
	//! the verb, and the effective principal to check it against (`who` = the
	//! enclosing definer view whose owner is checked, or null for the caller).
	//! Opaque to core: the access-control layer reads it and enforces. Verb bits
	//! mirror the privilege bitmask; columns are logical (0-based, PK-excluded).
	vector<AccessRequirement> access_requirements;
	//! Whether or not the statement requires a valid transaction. Almost all statements require this, with the
	//! exception of ROLLBACK
	bool requires_valid_transaction;
	//! Whether or not the result can be streamed to the client
	QueryResultOutputType output_type;
	//! Whether or not all parameters have successfully had their types determined
	bool bound_all_parameters;
	//! What type of data the statement returns
	StatementReturnType return_type;
	//! The number of prepared statement parameters
	idx_t parameter_count;
	//! Whether or not the statement ALWAYS requires a rebind
	bool always_require_rebind;

	bool IsReadOnly() {
		return modified_databases.empty();
	}

	void RegisterDBRead(Catalog &catalog, ClientContext &context);
	void RegisterDBModify(Catalog &catalog, ClientContext &context, DatabaseModificationType modification);
};

} // namespace duckdb
