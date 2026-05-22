if (NOT MINGW)
    duckdb_extension_load(iceberg
            SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/duckdb_iceberg
            INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/duckdb_iceberg/src/include
            LOAD_TESTS
            TEST_DIR ${CMAKE_SOURCE_DIR}/third_party/duckdb_iceberg/test
            )
endif()
