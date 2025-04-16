cmake_minimum_required(VERSION 3.16)
project(stack_usage_analyzer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)  # Optionnel mais recommandé

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(stack_usage_analyzer main.cpp)

llvm_map_components_to_libnames(llvm_libs
  core
  irreader
  support
)

target_link_libraries(stack_usage_analyzer PRIVATE ${llvm_libs})
