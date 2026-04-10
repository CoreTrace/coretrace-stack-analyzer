# SPDX-License-Identifier: Apache-2.0
set(CORETRACE_LOGGER_BUILD_EXAMPLES OFF CACHE BOOL "Disable logger examples" FORCE)
set(CORETRACE_LOGGER_BUILD_TESTS OFF CACHE BOOL "Disable logger tests" FORCE)

include(FetchContent)

FetchContent_Declare(coretrace_logger
  GIT_REPOSITORY https://github.com/CoreTrace/coretrace-log.git
  GIT_TAG        main
  EXCLUDE_FROM_ALL
)

FetchContent_GetProperties(coretrace_logger)
if(NOT coretrace_logger_POPULATED)
  FetchContent_Populate(coretrace_logger)
endif()

if(NOT TARGET coretrace_logger)
  add_subdirectory("${coretrace_logger_SOURCE_DIR}" "${coretrace_logger_BINARY_DIR}" EXCLUDE_FROM_ALL)
endif()
