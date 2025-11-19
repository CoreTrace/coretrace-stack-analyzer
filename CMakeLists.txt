cmake_minimum_required(VERSION 3.16)
project(stack_usage_analyzer)

include(${CMAKE_SOURCE_DIR}/cmake/CheckLLVMVersion.cmake)
set(LLVM_MIN_REQUIRED_VERSION "19" CACHE STRING "Minimum required LLVM version")
check_llvm_version(${LLVM_MIN_REQUIRED_VERSION})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)  # Optionnel mais recommandé

set(LLVM_LINK_LLVM_DYLIB ON)

find_package(LLVM REQUIRED CONFIG)

include(FetchContent)

FetchContent_Declare(
  cc
  GIT_REPOSITORY https://github.com/CoreTrace/coretrace-compiler.git
  GIT_TAG feat/ir-output-mode
  # GIT_TAG main
)

FetchContent_MakeAvailable(cc)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# llvm_map_components_to_libnames(llvm_libs
#   core
#   irreader
#   support
# )


# ===== LIBRARY =====
# Contient ta logique d'analyse (pas de main())
add_library(stack_usage_analyzer_lib
    src/StackUsageAnalyzer.cpp
)

target_include_directories(stack_usage_analyzer_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LLVM_INCLUDE_DIRS}
)

target_link_libraries(stack_usage_analyzer_lib
    PUBLIC
        ${llvm_libs}
        cc::compilerlib_static
)

# ===== CLI BINARY =====
add_executable(stack_usage_analyzer
  main.cpp
)

# target_link_libraries(stack_usage_analyzer PRIVATE ${llvm_libs})
target_link_libraries(stack_usage_analyzer
  PRIVATE
    stack_usage_analyzer_lib
    cc::compilerlib_static
)
