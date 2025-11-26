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

# Options de build
option(BUILD_CLI "Build stack_usage_analyzer CLI tool" ON)
option(BUILD_SHARED_LIB "Build shared library variant" ON)

# ===========================
#  Sources communs
# ===========================
set(STACK_ANALYZER_SOURCES
    src/StackUsageAnalyzer.cpp
    src/mangle.cpp
)

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# llvm_map_components_to_libnames(llvm_libs
#   core
#   irreader
#   support
# )

# ===== LIBRARY =====
add_library(stack_usage_analyzer_lib
    ${STACK_ANALYZER_SOURCES}
)

# target_include_directories(stack_usage_analyzer_lib
#     PUBLIC
#         ${CMAKE_CURRENT_SOURCE_DIR}/include
#         ${LLVM_INCLUDE_DIRS}
# )

# FOR USE WITH FETCHCONTENT
target_include_directories(stack_usage_analyzer_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        ${LLVM_INCLUDE_DIRS}
)

# ALIAS FOR USE WITH FETCHCONTENT
add_library(coretrace::stack_usage_analyzer_lib ALIAS stack_usage_analyzer_lib)

# Replace this one :
target_link_libraries(stack_usage_analyzer_lib
    PUBLIC
        ${llvm_libs}
        cc::compilerlib_static
)
# by this one :
if(LLVM_LINK_LLVM_DYLIB)
    target_link_libraries(stack_usage_analyzer_lib
        PUBLIC
            LLVM
            cc::compilerlib_static
    )
else()
    llvm_map_components_to_libnames(llvm_libs
      core
      irreader
      support
    )
    target_link_libraries(stack_usage_analyzer_lib
        PUBLIC
            ${llvm_libs}
            cc::compilerlib_static
    )
endif()

# # ===== CLI BINARY =====
# add_executable(stack_usage_analyzer
#   main.cpp
# )

# # target_link_libraries(stack_usage_analyzer PRIVATE ${llvm_libs})
# target_link_libraries(stack_usage_analyzer
#   PRIVATE
#     stack_usage_analyzer_lib
#     cc::compilerlib_static
# )

# ===== CLI BINARY =====
if(BUILD_CLI)
    add_executable(stack_usage_analyzer
      main.cpp
    )

    target_link_libraries(stack_usage_analyzer
      PRIVATE
        stack_usage_analyzer_lib
        # pas besoin de relinker cc::compilerlib_static ici,
        # il est déjà dans la lib
    )
endif()
