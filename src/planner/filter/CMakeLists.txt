add_library_unity(
  duckdb_planner_filter
  OBJECT
  expression_filter.cpp
  table_filter_bloom_function.cpp
  table_filter_dynamic_function.cpp
  table_filter_functions.cpp
  table_filter_optional_function.cpp
  table_filter_prefix_range_function.cpp
  table_filter_selectivity_optional_function.cpp
  zonemap_checker.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_planner_filter>
    PARENT_SCOPE)
