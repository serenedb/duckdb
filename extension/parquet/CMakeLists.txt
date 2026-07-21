cmake_minimum_required(VERSION 3.5...3.29)

project(ParquetExtension)

include_directories(include ../../third_party/parquet ../../third_party/thrift)

add_subdirectory(decoder)
add_subdirectory(reader)
add_subdirectory(writer)
set(PARQUET_EXTENSION_FILES
    ${PARQUET_EXTENSION_FILES}
    column_reader.cpp
    column_writer.cpp
    parquet_crypto.cpp
    parquet_extension.cpp
    parquet_file_metadata_cache.cpp
    parquet_float16.cpp
    parquet_multi_file_info.cpp
    parquet_metadata.cpp
    parquet_prefetch_cost_model.cpp
    parquet_reader.cpp
    parquet_field_id.cpp
    parquet_statistics.cpp
    parquet_timestamp.cpp
    parquet_writer.cpp
    parquet_shredding.cpp
    parquet_column_schema.cpp
    parquet_geometry.cpp
    serialize_parquet.cpp
    zstd_file_system.cpp)

if(NOT CLANG_TIDY)
  # parquet/thrift
  set(PARQUET_EXTENSION_FILES
      ${PARQUET_EXTENSION_FILES}
      ../../third_party/parquet/parquet_types.cpp
      ../../third_party/thrift/thrift/protocol/TProtocol.cpp
      ../../third_party/thrift/thrift/transport/TTransportException.cpp
      ../../third_party/thrift/thrift/transport/TBufferTransports.cpp)
endif()

build_static_extension(parquet ${PARQUET_EXTENSION_FILES})
target_link_libraries(parquet_extension snappy lz4_static brotlienc brotlidec)
set(PARAMETERS "-warnings")

build_loadable_extension(parquet ${PARAMETERS} ${PARQUET_EXTENSION_FILES})
target_link_libraries(parquet_loadable_extension duckdb_mbedtls snappy
                      lz4_static brotlienc brotlidec)

install(
  TARGETS parquet_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
