find_package(Threads REQUIRED)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)

add_library(duckdb_fsst STATIC libfsst.cpp fsst_simd.cpp fsst_avx512.cpp)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64" AND NOT MSVC)
  set_source_files_properties(fsst_avx512.cpp PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512bw;-mavx512dq")
endif()

target_include_directories(duckdb_fsst PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
set_target_properties(duckdb_fsst PROPERTIES EXPORT_NAME duckdb_fsst)

install(TARGETS duckdb_fsst
        EXPORT "${DUCKDB_EXPORT_SET}"
        LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
        ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

disable_target_warnings(duckdb_fsst)