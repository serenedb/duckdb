add_library_unity(
  test_arrow_roundtrip
  OBJECT
  arrow_test_helper.cpp
  arrow_roundtrip.cpp
  arrow_filter_pushdown.cpp
  arrow_output_version_buffers.cpp)
add_library(
  test_arrow_nanoarrow OBJECT
  arrow_move_children.cpp arrow_dictionary_sentinel.cpp
  arrow_dict_nested_offset.cpp arrow_list_view_nested_offset.cpp)
if(TARGET nanoarrow::nanoarrow)
  target_link_libraries(test_arrow_nanoarrow PRIVATE nanoarrow::nanoarrow)
endif()
set(UNITTEST_OBJECT_FILES
    ${UNITTEST_OBJECT_FILES} $<TARGET_OBJECTS:test_arrow_roundtrip>
    $<TARGET_OBJECTS:test_arrow_nanoarrow>
    PARENT_SCOPE)
