add_library_unity(test_catalog OBJECT test_catalog_version.cpp
                  test_postgres_schema_qualified_types.cpp)

set(UNITTEST_OBJECT_FILES
    ${UNITTEST_OBJECT_FILES} $<TARGET_OBJECTS:test_catalog>
    PARENT_SCOPE)
