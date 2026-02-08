cmake_minimum_required(VERSION 3.16)
project(stack_usage_analyzer)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(CheckLLVMVersion OPTIONAL)

if(COMMAND check_llvm_version)
    set(LLVM_MIN_REQUIRED_VERSION "19" CACHE STRING "Minimum required LLVM version")
    check_llvm_version(${LLVM_MIN_REQUIRED_VERSION})
else()
    message(WARNING "check_llvm_version() not available, skipping LLVM version check")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(LLVM_LINK_LLVM_DYLIB ON)

find_package(LLVM REQUIRED CONFIG)

include(FetchContent)

# Optional ASAN enablement at the top-level to match the cc dependency.
option(ENABLE_DEBUG_ASAN "Enable debug symbols and AddressSanitizer" OFF)
if(DEFINED DEBUG_ASAN)
    set(ENABLE_DEBUG_ASAN ${DEBUG_ASAN} CACHE BOOL
        "Enable debug symbols and AddressSanitizer" FORCE)
endif()

FetchContent_Declare(
    cc
    GIT_REPOSITORY https://github.com/CoreTrace/coretrace-compiler.git
    GIT_TAG main
)

FetchContent_MakeAvailable(cc)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# Options de build
option(BUILD_CLI "Build stack_usage_analyzer CLI tool" ON)
option(BUILD_SHARED_LIB "Build shared library variant" ON)

# ===========================
#  Communs Sources
# ===========================
set(STACK_ANALYZER_SOURCES
    src/StackUsageAnalyzer.cpp
    src/analysis/AllocaUsage.cpp
    src/analysis/AnalyzerUtils.cpp
    src/analysis/CompileCommands.cpp
    src/analysis/ConstParamAnalysis.cpp
    src/analysis/DuplicateIfCondition.cpp
    src/analysis/DynamicAlloca.cpp
    src/analysis/FunctionFilter.cpp
    src/analysis/IRValueUtils.cpp
    src/analysis/IntRanges.cpp
    src/analysis/InputPipeline.cpp
    src/analysis/InvalidBaseReconstruction.cpp
    src/analysis/MemIntrinsicOverflow.cpp
    src/analysis/SizeMinusKWrites.cpp
    src/analysis/StackBufferAnalysis.cpp
    src/analysis/StackComputation.cpp
    src/analysis/StackPointerEscape.cpp
    src/report/ReportSerialization.cpp
    src/mangle.cpp
    src/passes/ModulePasses.cpp
)

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# llvm_map_components_to_libnames(llvm_libs
#   core
#   irreader
#   support
# )

# ===== LIBRARY =====
add_library(stack_usage_analyzer_lib STATIC
    ${STACK_ANALYZER_SOURCES}
)

if(ENABLE_DEBUG_ASAN)
    target_compile_options(stack_usage_analyzer_lib PUBLIC -fsanitize=address -fno-omit-frame-pointer -g)
    target_link_options(stack_usage_analyzer_lib PUBLIC -fsanitize=address)
endif()

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
        # No need to relink cc::compilerlib_static here,
        # it is already linked into the library.
    )

    if(ENABLE_DEBUG_ASAN)
        target_compile_options(stack_usage_analyzer PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
        target_link_options(stack_usage_analyzer PRIVATE -fsanitize=address)
    endif()
endif()

# ============
#  FORMATTING
# ============
# Only add these helper targets when building this project standalone to avoid
# collisions when consumed via FetchContent.
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  add_custom_target(format
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/format.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Run clang-format on source files")

  add_custom_target(format-check
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/format-check.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Verify clang-format compliance")
endif()
