set(CORETRACE_LOGGER_BUILD_EXAMPLES OFF CACHE BOOL "Disable logger examples" OFF)
set(CORETRACE_LOGGER_BUILD_TESTS OFF CACHE BOOL "Disable logger tests" OFF)

include(FetchContent)

FetchContent_Declare(coretrace-logger
  GIT_REPOSITORY https://github.com/CoreTrace/coretrace-log.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(coretrace-logger)
