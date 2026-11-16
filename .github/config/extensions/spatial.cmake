duckdb_extension_load(spatial
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../duckdb_spatial
        INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../duckdb_spatial/src/spatial
        LOAD_TESTS
        TEST_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../duckdb_spatial/test/sql
        )
