cmake_minimum_required(VERSION 3.16)
project(consumer_example CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
  stack_analyzer
  GIT_REPOSITORY https://github.com/CoreTrace/coretrace-stack-analyzer.git
  GIT_TAG feature/stack-buffer-bounds-check
#   GIT_TAG main
)

FetchContent_MakeAvailable(stack_analyzer)

add_executable(sa_consumer src/main.cpp)

target_link_libraries(sa_consumer
    PRIVATE
        coretrace::stack_usage_analyzer_lib
)
