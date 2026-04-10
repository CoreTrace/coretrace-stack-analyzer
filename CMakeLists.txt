# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.21)
cmake_policy(SET CMP0091 NEW)
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()
project(stack_usage_analyzer)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(CheckLLVMVersion OPTIONAL)
include(CheckCXXCompilerFlag)

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

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

function(coretrace_force_msvc_runtime target_name)
    if(MSVC AND TARGET "${target_name}")
        set_property(TARGET "${target_name}" PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endfunction()

function(coretrace_patch_imported_link_property target_name property_name stale_value replacement_value)
    if(NOT TARGET "${target_name}")
        return()
    endif()

    get_target_property(current_value "${target_name}" "${property_name}")
    if(NOT current_value OR current_value STREQUAL "current_value-NOTFOUND")
        return()
    endif()

    string(REPLACE "${stale_value}" "${replacement_value}" updated_value "${current_value}")
    if(NOT updated_value STREQUAL current_value)
        set_target_properties("${target_name}" PROPERTIES
            ${property_name} "${updated_value}")
    endif()
endfunction()

function(coretrace_patch_llvm_diaguids replacement_path)
    if(NOT WIN32 OR NOT TARGET LLVMDebugInfoPDB)
        return()
    endif()

    set(coretrace_stale_diaguids_path
        "C:/Program Files (x86)/Microsoft Visual Studio/2019/Professional/DIA SDK/lib/amd64/diaguids.lib")

    foreach(property_name
            INTERFACE_LINK_LIBRARIES
            IMPORTED_LINK_INTERFACE_LIBRARIES
            IMPORTED_LINK_INTERFACE_LIBRARIES_DEBUG
            IMPORTED_LINK_INTERFACE_LIBRARIES_RELEASE
            IMPORTED_LINK_INTERFACE_LIBRARIES_RELWITHDEBINFO
            IMPORTED_LINK_INTERFACE_LIBRARIES_MINSIZEREL)
        coretrace_patch_imported_link_property(
            LLVMDebugInfoPDB
            ${property_name}
            "${coretrace_stale_diaguids_path}"
            "${replacement_path}")
    endforeach()
endfunction()

if(WIN32)
    set(LLVM_LINK_LLVM_DYLIB OFF)
else()
    set(LLVM_LINK_LLVM_DYLIB ON)
endif()

find_package(LLVM REQUIRED CONFIG)

if(WIN32)
    set(coretrace_diaguids_candidates)
    file(GLOB_RECURSE coretrace_diaguids_candidates
        LIST_DIRECTORIES false
        "C:/Program Files/Microsoft Visual Studio/*/*/DIA SDK/lib/amd64/diaguids.lib"
        "C:/Program Files (x86)/Microsoft Visual Studio/*/*/DIA SDK/lib/amd64/diaguids.lib")

    if(coretrace_diaguids_candidates)
        list(GET coretrace_diaguids_candidates 0 coretrace_diaguids_path)
        coretrace_patch_llvm_diaguids("${coretrace_diaguids_path}")
    endif()
endif()

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# ===========================
# INCLUDE .CMAKE
# ===========================

include(${CMAKE_CURRENT_LIST_DIR}/cmake/compiler/coretrace-compiler.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/logger/coretraceLog.cmake)

# ===========================
#  Build Options
# ===========================

option(BUILD_CLI "Build stack_usage_analyzer CLI tool" ON)
option(BUILD_SHARED_LIB "Build shared library variant" ON)
option(ENABLE_STACK_USAGE "Emit per-function stack usage (.su) files" ON)
option(ENABLE_WARN_PADDED "Enable -Wpadded warnings" ON)
option(ENABLE_WARN_REORDER_INIT_LIST "Enable -Wreorder-init-list when supported" ON)
option(ENABLE_Z3_BACKEND "Enable optional Z3 SMT backend if Z3 is available" ON)

message("Building CLI Tool                      = ${BUILD_CLI}")
message("Building Shared Library                = ${BUILD_SHARED_LIB}")
message("Emitting Stack Usage (.su) Files       = ${ENABLE_STACK_USAGE}")
message("Enabling -Wpadded Warnings             = ${ENABLE_WARN_PADDED}")
message("Enabling -Wreorder-init-list           = ${ENABLE_WARN_REORDER_INIT_LIST}")
message("Enabling Z3 SMT Backend                = ${ENABLE_Z3_BACKEND}")


if(ENABLE_WARN_REORDER_INIT_LIST)
    check_cxx_compiler_flag("-Wreorder-init-list" CTRACE_STACK_HAS_WREORDER_INIT_LIST)
endif()

# ===========================
#  Communs Sources
# ===========================
set(STACK_ANALYZER_SOURCES
    src/analyzer/AnalysisPipeline.cpp
    src/analyzer/DiagnosticEmitter.cpp
    src/analyzer/HotspotProfiler.cpp
    src/analyzer/IRFactCollector.cpp
    src/analyzer/LocationResolver.cpp
    src/analyzer/PerFunctionInstructionCache.cpp
    src/analyzer/ModulePreparationService.cpp
    src/app/AnalyzerApp.cpp
    src/cli/ArgParser.cpp
    src/StackUsageAnalyzer.cpp
    src/analysis/AllocaUsage.cpp
    src/analysis/AnalyzerUtils.cpp
    src/analysis/CompileCommands.cpp
    src/analysis/ConstParamAnalysis.cpp
    src/analysis/CommandInjectionAnalysis.cpp
    src/analysis/DuplicateIfCondition.cpp
    src/analysis/DynamicAlloca.cpp
    src/analysis/BufferWriteModel.cpp
    src/analysis/FrontendDiagnostics.cpp
    src/analysis/FunctionFilter.cpp
    src/analysis/GlobalReadBeforeWriteAnalysis.cpp
    src/analysis/IRValueUtils.cpp
    src/analysis/IntRanges.cpp
    src/analysis/IntegerOverflowAnalysis.cpp
    src/analysis/InputPipeline.cpp
    src/analysis/InvalidBaseReconstruction.cpp
    src/analysis/MemIntrinsicOverflow.cpp
    src/analysis/NullDerefAnalysis.cpp
    src/analysis/OOBReadAnalysis.cpp
    src/analysis/ParameterDebugBinding.cpp
    src/analysis/ResourceLifetimeAnalysis.cpp
    src/analysis/Reachability.cpp
    src/analysis/SizeMinusKWrites.cpp
    src/analysis/StackBufferAnalysis.cpp
    src/analysis/StackComputation.cpp
    src/analysis/StackPointerEscape.cpp
    src/analysis/StackPointerEscapeModel.cpp
    src/analysis/StackPointerEscapeResolver.cpp
    src/analysis/smt/SmtEncoding.cpp
    src/analysis/smt/SolverOrchestrator.cpp
    src/analysis/TOCTOUAnalysis.cpp
    src/analysis/TypeConfusionAnalysis.cpp
    src/analysis/UninitializedVarAnalysis.cpp
    src/report/ReportSerialization.cpp
    src/mangle.cpp
    src/passes/ModulePasses.cpp
)

set(CTRACE_STACK_HAVE_Z3_BACKEND OFF)
if(ENABLE_Z3_BACKEND)
    find_package(Z3 QUIET CONFIG)
    if(Z3_FOUND)
        list(APPEND STACK_ANALYZER_SOURCES src/analysis/smt/backends/Z3Backend.cpp)
        set(CTRACE_STACK_HAVE_Z3_BACKEND ON)
        message(STATUS "SMT Z3 backend enabled")
    else()
        message(STATUS "SMT Z3 backend disabled (Z3 not found)")
    endif()
endif()

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

if(ENABLE_WARN_REORDER_INIT_LIST AND CTRACE_STACK_HAS_WREORDER_INIT_LIST)
    target_compile_options(stack_usage_analyzer_lib PRIVATE -Wreorder-init-list)
endif()

if(CTRACE_STACK_HAVE_Z3_BACKEND)
    target_compile_definitions(stack_usage_analyzer_lib PUBLIC CTRACE_STACK_ENABLE_Z3_BACKEND=1)
    if(TARGET z3::libz3)
        target_link_libraries(stack_usage_analyzer_lib PUBLIC z3::libz3)
    elseif(TARGET z3)
        target_link_libraries(stack_usage_analyzer_lib PUBLIC z3)
    elseif(DEFINED Z3_LIBRARIES)
        target_link_libraries(stack_usage_analyzer_lib PUBLIC ${Z3_LIBRARIES})
        if(DEFINED Z3_INCLUDE_DIRS)
            target_include_directories(stack_usage_analyzer_lib SYSTEM PUBLIC ${Z3_INCLUDE_DIRS})
        endif()
    endif()
endif()

if(ENABLE_STACK_USAGE AND NOT MSVC)
    target_compile_options(stack_usage_analyzer_lib PRIVATE -fstack-usage)
endif()

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
)

target_include_directories(stack_usage_analyzer_lib SYSTEM
    PUBLIC
        ${LLVM_INCLUDE_DIRS}
)

# Treat external dependency headers as system includes to reduce warning noise
# from third-party code when project warning levels are strict.
target_include_directories(stack_usage_analyzer_lib SYSTEM
    PUBLIC
        $<TARGET_PROPERTY:cc::compilerlib_static,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:coretrace::logger,INTERFACE_INCLUDE_DIRECTORIES>
)

# ALIAS FOR USE WITH FETCHCONTENT
add_library(coretrace::stack_usage_analyzer_lib ALIAS stack_usage_analyzer_lib)
coretrace_force_msvc_runtime(stack_usage_analyzer_lib)

# Replace this one :
target_link_libraries(stack_usage_analyzer_lib
    PUBLIC
        ${llvm_libs}
        cc::compilerlib_static
        coretrace::logger
)
# by this one :
if(LLVM_LINK_LLVM_DYLIB)
    target_link_libraries(stack_usage_analyzer_lib
        PUBLIC
            LLVM
            cc::compilerlib_static
            coretrace::logger
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
            coretrace::logger
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

    if(ENABLE_WARN_REORDER_INIT_LIST AND CTRACE_STACK_HAS_WREORDER_INIT_LIST)
        target_compile_options(stack_usage_analyzer PRIVATE -Wreorder-init-list)
    endif()

    target_link_libraries(stack_usage_analyzer
      PRIVATE
        stack_usage_analyzer_lib
        # No need to relink cc::compilerlib_static here,
        # it is already linked into the library.
    )

    if(ENABLE_STACK_USAGE AND NOT MSVC)
        target_compile_options(stack_usage_analyzer PRIVATE -fstack-usage)
    endif()

    if(ENABLE_WARN_PADDED AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(stack_usage_analyzer_lib PRIVATE -Wpadded)
        target_compile_options(stack_usage_analyzer PRIVATE -Wpadded)
    endif()

    if(ENABLE_DEBUG_ASAN)
        target_compile_options(stack_usage_analyzer PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
        target_link_options(stack_usage_analyzer PRIVATE -fsanitize=address)
    endif()

    coretrace_force_msvc_runtime(stack_usage_analyzer)
endif()

include(GNUInstallDirs)

install(TARGETS stack_usage_analyzer_lib
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

if(BUILD_CLI)
    install(TARGETS stack_usage_analyzer
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# =========
#  TESTING
# =========
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  include(CTest)

  option(BUILD_ANALYZER_UNIT_TESTS "Build fine-grained analyzer module unit tests" OFF)
  if(BUILD_ANALYZER_UNIT_TESTS)
    add_executable(stack_usage_analyzer_unit_tests
      test/unit/analyzer_module_unit_tests.cpp
    )

    target_link_libraries(stack_usage_analyzer_unit_tests
      PRIVATE
        stack_usage_analyzer_lib
    )

    add_test(
      NAME analyzer_module_unit_tests
      COMMAND stack_usage_analyzer_unit_tests ${CMAKE_CURRENT_SOURCE_DIR}
    )
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
