//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/wal_type.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

enum class WALType : uint8_t {
	INVALID = 0,
	// -----------------------------
	// Catalog
	// -----------------------------
	CREATE_TABLE = 1,
	DROP_TABLE = 2,

	CREATE_SCHEMA = 3,
	DROP_SCHEMA = 4,

	CREATE_VIEW = 5,
	DROP_VIEW = 6,

	CREATE_SEQUENCE = 8,
	DROP_SEQUENCE = 9,
	SEQUENCE_VALUE = 10,

	CREATE_MACRO = 11,
	DROP_MACRO = 12,

	CREATE_TYPE = 13,
	DROP_TYPE = 14,

	ALTER_INFO = 20,

	CREATE_TABLE_MACRO = 21,
	DROP_TABLE_MACRO = 22,

	CREATE_INDEX = 23,
	DROP_INDEX = 24,

	// -----------------------------
	// Data
	// -----------------------------
	USE_TABLE = 25,
	INSERT_TUPLE = 26,
	DELETE_TUPLE = 27,
	UPDATE_TUPLE = 28,
	ROW_GROUP_DATA = 29,

	CREATE_TRIGGER = 30,
	DROP_TRIGGER = 31,

	CREATE_TOKENIZER = 200,
	DROP_TOKENIZER = 201,
	CREATE_ROLE = 202,
	DROP_ROLE = 203,
	CREATE_DATABASE = 204,
	DROP_DATABASE = 205,
	CREATE_FOREIGN_SERVER = 206,
	DROP_FOREIGN_SERVER = 207,
	USE_CATALOG = 208,
	ARTIFACT = 211,
	CREATE_JOB = 212,
	DROP_JOB = 213,
	CREATE_POLICY = 214,
	DROP_POLICY = 215,
	// -----------------------------
	// Flush
	// -----------------------------
	WAL_VERSION = 98,
	CHECKPOINT = 99,
	WAL_FLUSH = 100,
	WAL_PREPARED = 209,
	COMMIT_PREPARED = 210
};
}
