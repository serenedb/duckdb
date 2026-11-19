#include "reader/byte_array_length_column_reader.hpp"

#include "parquet_column_schema.hpp"
#include "parquet_statistics.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

namespace duckdb {

ByteArrayLengthColumnReader::ByteArrayLengthColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema)
    : ColumnReader(reader, schema), fixed_width_string_length(DConstants::INVALID_INDEX) {
	if (schema.parquet_type == Type::FIXED_LEN_BYTE_ARRAY) {
		fixed_width_string_length = schema.type_length;
	}
}

unique_ptr<BaseStatistics> ByteArrayLengthColumnReader::Stats(idx_t, const vector<ColumnChunk> &columns) {
	const auto column_index = ColumnIndex();
	if (column_index >= columns.size() || !columns[column_index].__isset.meta_data ||
	    !columns[column_index].meta_data.__isset.statistics) {
		return nullptr;
	}
	auto &column = columns[column_index];
	return LengthStats(column.meta_data.statistics, &column);
}

unique_ptr<BaseStatistics> ByteArrayLengthColumnReader::PageStats(const duckdb_parquet::Statistics &page_stats) const {
	return LengthStats(page_stats, nullptr);
}

unique_ptr<BaseStatistics> ByteArrayLengthColumnReader::LengthStats(const duckdb_parquet::Statistics &statistics,
                                                                    optional_ptr<const ColumnChunk> column) const {
	idx_t min_length = 0;
	idx_t max_length = NumericLimits<uint32_t>::Maximum();
	if (fixed_width_string_length != DConstants::INVALID_INDEX) {
		min_length = max_length = fixed_width_string_length;
	} else {
		// the lengths follow from the statistics of the byte arrays, as a reader of the byte arrays would see them
		auto byte_arrays =
		    ParquetStatisticsUtils::TransformParquetStatistics(LogicalType::BLOB, Schema(), statistics, false, column);
		idx_t min_bytes;
		idx_t max_bytes;
		if (byte_arrays && StringStats::TryGetLengthRange(*byte_arrays, min_bytes, max_bytes)) {
			min_length = min_bytes;
			max_length = max_bytes;
		}
	}
	auto result = NumericStats::CreateUnknown(LogicalType::BIGINT);
	NumericStats::SetMin(result, Value::BIGINT(NumericCast<int64_t>(min_length)));
	NumericStats::SetMax(result, Value::BIGINT(NumericCast<int64_t>(max_length)));
	if (statistics.__isset.null_count && statistics.null_count == 0) {
		result.Set(StatsInfo::CANNOT_HAVE_NULL_VALUES);
	}
	return result.ToUnique();
}

void ByteArrayLengthColumnReader::Plain(shared_ptr<ResizeableBuffer> &plain_data, uint8_t *defines, idx_t num_values,
                                        idx_t result_offset, Vector &result) {
	PlainTemplated<int64_t, ByteArrayLengthValueConversion>(*plain_data, defines, num_values, result_offset, result);
}

void ByteArrayLengthColumnReader::PlainSkip(ByteBuffer &plain_data, uint8_t *defines, idx_t num_values) {
	PlainSkipTemplated<ByteArrayLengthValueConversion>(plain_data, defines, num_values);
}

void ByteArrayLengthColumnReader::PlainSelect(shared_ptr<ResizeableBuffer> &plain_data, uint8_t *defines,
                                              idx_t num_values, Vector &result, const SelectionVector &sel, idx_t count,
                                              idx_t result_offset) {
	PlainSelectTemplated<int64_t, ByteArrayLengthValueConversion>(*plain_data, defines, num_values, result, sel, count,
	                                                              result_offset);
}

} // namespace duckdb
