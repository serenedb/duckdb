add_library_unity(duckdb_formats OBJECT csv_scan.cpp csv_splitter.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_formats>
    PARENT_SCOPE)
