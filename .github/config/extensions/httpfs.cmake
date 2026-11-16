duckdb_extension_load(httpfs
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../duckdb_httpfs
    INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../duckdb_httpfs/src/include
    LOAD_TESTS
    TEST_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../duckdb_httpfs/test
)
