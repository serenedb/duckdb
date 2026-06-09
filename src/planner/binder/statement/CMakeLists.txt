add_library_unity(
  duckdb_bind_statement
  OBJECT
  bind_attach.cpp
  bind_call.cpp
  bind_connect.cpp
  bind_copy.cpp
  bind_copy_database.cpp
  bind_create.cpp
  bind_create_table.cpp
  bind_delete.cpp
  bind_detach.cpp
  bind_drop.cpp
  bind_execute.cpp
  bind_explain.cpp
  bind_export.cpp
  bind_extension.cpp
  bind_insert.cpp
  bind_logical_plan.cpp
  bind_merge_into.cpp
  bind_pragma.cpp
  bind_prepare.cpp
  bind_relation.cpp
  bind_select.cpp
  bind_set.cpp
  bind_simple.cpp
  bind_summarize.cpp
  bind_update.cpp
  bind_vacuum.cpp)

add_library(duckdb_bind_extension_load OBJECT bind_load.cpp
                                              bind_update_extensions.cpp)

if(BUILD_UNITTESTS)
  add_library(duckdb_bind_extension_load_testload OBJECT
              bind_load.cpp bind_update_extensions.cpp)
  target_compile_definitions(duckdb_bind_extension_load_testload
                             PRIVATE SDB_ENABLE_TEST_EXTENSION_LOAD)
endif()

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_bind_statement>
    PARENT_SCOPE)
