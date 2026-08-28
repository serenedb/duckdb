if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(spatial
            SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/duckdb_spatial
            INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/duckdb_spatial/src/spatial
            LOAD_TESTS
            TEST_DIR ${CMAKE_SOURCE_DIR}/third_party/duckdb_spatial/test/sql
            )
endif()
