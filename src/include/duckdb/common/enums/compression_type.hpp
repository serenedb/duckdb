//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/compression_type.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {

class StorageManager;

enum class CompressionType : uint8_t {
	COMPRESSION_AUTO = 0,
	COMPRESSION_UNCOMPRESSED = 1,
	COMPRESSION_CONSTANT = 2, // internal only
	COMPRESSION_RLE = 3,
	COMPRESSION_DICTIONARY = 4,
	COMPRESSION_PFOR_DELTA = 5,
	COMPRESSION_BITPACKING = 6,
	COMPRESSION_FSST = 7,
	COMPRESSION_CHIMP = 8,
	COMPRESSION_PATAS = 9,
	COMPRESSION_ALP = 10,
	COMPRESSION_ALPRD = 11,
	COMPRESSION_ZSTD = 12,
	COMPRESSION_ROARING = 13,
	COMPRESSION_EMPTY = 14, // internal only
	COMPRESSION_DICT_FSST = 15,
	COMPRESSION_COUNT, // This has to stay the last entry of the type!
	COMPRESSION_DICT_LZ4 = 100,
	COMPRESSION_DICT_ZSTD = 101,
	COMPRESSION_LZ4 = 102,
	// the columnstore's own dict_fsst / fsst; they carry the user-facing names of their duckdb namesakes, which
	// a search table maps onto them
	COMPRESSION_COL_DICT_FSST = 103,
	COMPRESSION_COL_FSST = 104,
	COMPRESSION_DICT_ZXC = 105,
	COMPRESSION_ZXC = 106,
	COMPRESSION_COL_ZSTD = 107,
	COMPRESSION_SERENEDB_COUNT // This has to stay the last SereneDB entry!
};

inline bool IsSereneDBCompressionType(CompressionType type) {
	return type >= CompressionType::COMPRESSION_DICT_LZ4 && type < CompressionType::COMPRESSION_SERENEDB_COUNT;
}

struct CompressionAvailabilityResult {
private:
	enum class UnavailableReason : uint8_t {
		AVAILABLE,
		//! Introduced later, not available to this version
		NOT_AVAILABLE_YET,
		//! Used to be available, but isnt anymore
		DEPRECATED
	};

public:
	CompressionAvailabilityResult() = default;
	static CompressionAvailabilityResult Deprecated() {
		return CompressionAvailabilityResult(UnavailableReason::DEPRECATED);
	}
	static CompressionAvailabilityResult NotAvailableYet() {
		return CompressionAvailabilityResult(UnavailableReason::NOT_AVAILABLE_YET);
	}

public:
	bool IsAvailable() const {
		return reason == UnavailableReason::AVAILABLE;
	}
	bool IsDeprecated() {
		D_ASSERT(!IsAvailable());
		return reason == UnavailableReason::DEPRECATED;
	}
	bool IsNotAvailableYet() {
		D_ASSERT(!IsAvailable());
		return reason == UnavailableReason::NOT_AVAILABLE_YET;
	}

private:
	explicit CompressionAvailabilityResult(UnavailableReason reason) : reason(reason) {
	}

public:
	UnavailableReason reason = UnavailableReason::AVAILABLE;
};

CompressionAvailabilityResult CompressionTypeIsAvailable(CompressionType compression_type,
                                                         optional_ptr<StorageManager> storage_manager = nullptr);
vector<string> ListCompressionTypes(void);
string CompressionTypeToString(CompressionType type);

} // namespace duckdb
