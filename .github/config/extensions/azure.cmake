if (NOT MINGW AND NOT ${WASM_ENABLED})
  duckdb_extension_load(azure
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../duckdb_azure
        INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../duckdb_azure/src/include
        LOAD_TESTS
        TEST_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../duckdb_azure/test
  )
endif()
