#include "duckdb/common/serializer/serializer.hpp"

namespace duckdb {

void Serializer::List::WriteElement(data_ptr_t ptr, idx_t size) {
	serializer.WriteDataPtr(ptr, size);
}

} // namespace duckdb
