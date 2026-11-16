if (NOT MINGW)
    duckdb_extension_load(avro
            SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../duckdb_avro
            INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../duckdb_avro/src/include
            LOAD_TESTS
            TEST_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../duckdb_avro/test
    )
endif()
