if (NOT MINGW AND NOT ${WASM_ENABLED})
  duckdb_extension_load(azure
        LOAD_TESTS
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/duckdb_azure
        INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/duckdb_azure/src/include
  )
endif()
