duckdb_extension_load(inet
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../duckdb_inet
    INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../duckdb_inet/src/duckdb/inet
    LOAD_TESTS
    TEST_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../duckdb_inet/test
    )
