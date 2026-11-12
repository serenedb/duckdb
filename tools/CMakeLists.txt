if(NOT SUN AND (BUILD_SHELL OR BUILD_SHELL_STATIC_LIB))
  add_subdirectory(shell)
endif()

add_subdirectory(cpp)
