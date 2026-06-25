if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(clickhouse_scanner
            SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/duckdb_clickhouse
            INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/duckdb_clickhouse/src/include
    )
endif()
