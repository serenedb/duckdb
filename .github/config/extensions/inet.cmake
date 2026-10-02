duckdb_extension_load(inet
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/duckdb_inet
    INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/duckdb_inet/src/duckdb/inet
    LOAD_TESTS
    TEST_DIR ${CMAKE_SOURCE_DIR}/third_party/duckdb_inet/test
    )
