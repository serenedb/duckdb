# Note: tests for postgres_scanner are currently not run. All of them need a postgres server running. One test
#       uses a remote rds server but that's not something we want to run here.
#
# SereneDB note: DONT_LINK removed so postgres_scanner is statically linked into
# duckdb and registered in the auto-loader (we point GIT_URL at our fork via
# the DUCKDB_POSTGRES_SCANNER_DIRECTORY env var, so the GIT_TAG below is
# advisory).
if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(postgres_scanner
            GIT_URL https://github.com/duckdb/duckdb-postgres
            GIT_TAG a42c490df0019406658073c003b7d89dd4338466
            APPLY_PATCHES
            )
endif()
