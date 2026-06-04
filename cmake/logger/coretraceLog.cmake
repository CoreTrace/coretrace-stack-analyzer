# SPDX-License-Identifier: Apache-2.0
set(CORETRACE_LOGGER_BUILD_EXAMPLES OFF CACHE BOOL "Disable logger examples" OFF)
set(CORETRACE_LOGGER_BUILD_TESTS OFF CACHE BOOL "Disable logger tests" OFF)

if(TARGET coretrace::logger)
  return()
endif()

if(TARGET coretrace_logger)
  add_library(coretrace::logger ALIAS coretrace_logger)
  return()
endif()

include(FetchContent)

FetchContent_Declare(coretrace_logger
  GIT_REPOSITORY https://github.com/CoreTrace/coretrace-log.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(coretrace_logger)
