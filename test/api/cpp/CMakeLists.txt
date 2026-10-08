# Tests for the stable C++ API. The sources live next to the API itself, in
# tools/cpp/tests, so the package carries its own tests; they are compiled into
# the unittest binary from here, which is where UNITTEST_OBJECT_FILES is
# assembled.
include_directories(${PROJECT_SOURCE_DIR}/tools/cpp
                    ${PROJECT_SOURCE_DIR}/tools/cpp/tests)

# Plain add_library rather than add_library_unity: the unity helper writes
# relative #includes into a generated file, which cannot reach out of the tree.
add_library(
  test_cpp_api OBJECT
  ${PROJECT_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_aggregate_function.cpp
  ${PROJECT_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_appender.cpp
  ${PROJECT_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_arrow.cpp
  ${PROJECT_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_cast_function.cpp
  ${PROJECT_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_catalog.cpp
  ${PROJECT_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_column_data_collection.cpp
  ${PROJECT_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_copy_function.cpp
  ${PROJECT_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_core.cpp
  ${PROJECT_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_file_system.cpp
  ${PROJECT_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_prepared_statement.cpp
  ${PROJECT_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_query.cpp
  ${PROJECT_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_replacement_scan.cpp
  ${PROJECT_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_scalar_function.cpp
  ${PROJECT_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_table_function.cpp
  ${PROJECT_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_tokenizer.cpp
  ${PROJECT_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_types_values.cpp
  ${PROJECT_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_vector.cpp)

set(UNITTEST_OBJECT_FILES
    ${UNITTEST_OBJECT_FILES} $<TARGET_OBJECTS:test_cpp_api>
    PARENT_SCOPE)
