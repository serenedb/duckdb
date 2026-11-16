if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(postgres_scanner
            SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../duckdb_postgres
            INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../duckdb_postgres/src/include
            LOAD_TESTS
            TEST_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../duckdb_postgres/test
    )
endif()
