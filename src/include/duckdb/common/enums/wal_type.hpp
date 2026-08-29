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

	// -----------------------------
	// SereneDB
	// -----------------------------
	//! A create/drop of a kind duckdb keeps no entry class for -- a serenedb database, role, tokenizer or foreign
	//! server. One pair of records rather than one per kind: the CreateInfo already says which kind it is, and the
	//! catalog that owns the kind is what applies it.
	CREATE_ENTRY = 42,
	DROP_ENTRY = 43,
	//! State a catalog keeps that is not an entry: a sequence's counter, the id horizon, an open drop. Opaque here --
	//! the catalog that wrote the payload is the only thing that can read it.
	CATALOG_STATE = 44,
	// -----------------------------
	// Flush
	// -----------------------------
	WAL_VERSION = 98,
	CHECKPOINT = 99,
	WAL_FLUSH = 100
};
}
