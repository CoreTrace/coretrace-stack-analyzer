cmake_minimum_required(VERSION 3.28)
project(consumer_example CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Prefer the local parent checkout when this example is built from the
# repository tree; fallback to GitHub when used standalone.
set(STACK_ANALYZER_LOCAL_SOURCE "${CMAKE_CURRENT_LIST_DIR}/..")
if(EXISTS "${STACK_ANALYZER_LOCAL_SOURCE}/CMakeLists.txt")
  message(STATUS "Using local stack analyzer source: ${STACK_ANALYZER_LOCAL_SOURCE}")
  FetchContent_Declare(
    stack_analyzer
    SOURCE_DIR "${STACK_ANALYZER_LOCAL_SOURCE}"
  )
else()
  message(STATUS "Using remote stack analyzer source from GitHub")
  FetchContent_Declare(
    stack_analyzer
    GIT_REPOSITORY https://github.com/CoreTrace/coretrace-stack-analyzer.git
    GIT_TAG main
  )
endif()

FetchContent_MakeAvailable(stack_analyzer)

add_executable(sa_consumer src/main.cpp)

target_link_libraries(sa_consumer
    PRIVATE
        coretrace::stack_usage_analyzer_lib
)
