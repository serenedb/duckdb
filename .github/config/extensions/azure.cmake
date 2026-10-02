if (NOT MINGW AND NOT ${WASM_ENABLED})
  duckdb_extension_load(azure
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/duckdb_azure
        INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/duckdb_azure/src/include
        LOAD_TESTS
        TEST_DIR ${CMAKE_SOURCE_DIR}/third_party/duckdb_azure/test
  )
endif()
