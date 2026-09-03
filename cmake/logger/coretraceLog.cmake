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

# Pinned to a commit SHA so source builds are reproducible and a compromised
# upstream default branch cannot silently enter a build. Bump deliberately.
FetchContent_Declare(coretrace_logger
  GIT_REPOSITORY https://github.com/CoreTrace/coretrace-log.git
  GIT_TAG        624688ad5e5d00a1d04fd72909d43fe6d948575b
)
FetchContent_MakeAvailable(coretrace_logger)
