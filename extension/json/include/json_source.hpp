//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_source.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/file_compression_type.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/formats/row_input.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

class JsonSource : public ScanFile {
public:
	JsonSource(ClientContext &context, OpenFileInfo file, FileCompressionType compression);
};

} // namespace duckdb
