add_library_unity(duckdb_block_cache OBJECT block_cache.cpp
                  block_cache_spill.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_block_cache>
    PARENT_SCOPE)
