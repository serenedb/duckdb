add_library_unity(duckdb_csv_scanner OBJECT csv_schema.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_csv_scanner>
    PARENT_SCOPE)
