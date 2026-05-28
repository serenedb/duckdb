include_directories(include)

add_definitions(-DHAVE_LINENOISE=1)
include_directories(../../third_party/utf8proc/include)
include_directories(linenoise/include)
add_definitions(-DUSE_DUCKDB_SHELL_WRAPPER)

include_directories(../../third_party/utf8proc/include)

if(_DUCKDB_PREBUILT_SHELL_PATH)
  add_library(duckdb_shell STATIC IMPORTED)
  set_property(TARGET duckdb_shell PROPERTY IMPORTED_LOCATION
                                            "${_DUCKDB_PREBUILT_SHELL_PATH}")
else()
  add_subdirectory(linenoise)
  set(SHELL_SOURCES
      shell_command_line_option.cpp
      shell_extension.cpp
      shell.cpp
      shell_helpers.cpp
      shell_manual.cpp
      shell_metadata_command.cpp
      shell_prompt.cpp
      shell_renderer.cpp
      shell_highlight.cpp
      shell_profiler_renderer.cpp
      shell_progress_bar.cpp
      shell_render_table_metadata.cpp
      shell_windows.cpp
      $<TARGET_OBJECTS:duckdb_linenoise>)

  add_library(duckdb_shell STATIC ${SHELL_SOURCES})
  set_target_properties(
    duckdb_shell
    PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/src"
               ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${PROJECT_BINARY_DIR}/src"
               ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${PROJECT_BINARY_DIR}/src"
               ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO
               "${PROJECT_BINARY_DIR}/src"
               ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL "${PROJECT_BINARY_DIR}/src")
  duckdb_make_native_lto_archive(duckdb_shell)

  target_include_directories(duckdb_shell PUBLIC include)
  target_link_libraries(duckdb_shell duckdb_static ${DUCKDB_EXTRA_LINK_FLAGS})
  link_threads(duckdb_shell "")
  if(NOT AMALGAMATION_BUILD AND NOT WIN32)
    target_link_libraries(duckdb_shell duckdb_utf8proc)
  endif()
endif()
