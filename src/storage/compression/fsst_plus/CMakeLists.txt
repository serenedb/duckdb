add_library_unity(duckdb_storage_compression_fsst_plus OBJECT analyze.cpp
                  compression.cpp decompression.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_storage_compression_fsst_plus>
    PARENT_SCOPE)
