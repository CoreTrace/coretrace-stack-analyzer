# SPDX-License-Identifier: Apache-2.0
include(FetchContent)

# Optional ASAN enablement at the top-level to match the cc dependency.
option(ENABLE_DEBUG_ASAN "Enable debug symbols and AddressSanitizer" OFF)
if(DEFINED DEBUG_ASAN)
    set(ENABLE_DEBUG_ASAN ${DEBUG_ASAN} CACHE BOOL
        "Enable debug symbols and AddressSanitizer" FORCE)
endif()

# Pinned to a commit SHA so source builds are reproducible and a compromised
# upstream default branch cannot silently enter a build. Bump deliberately.
FetchContent_Declare(
    cc
    GIT_REPOSITORY https://github.com/CoreTrace/coretrace-compiler.git
    GIT_TAG b4ad33638e9020a93f052a7a299e4105bd48f975 # v0.10.0
)
FetchContent_MakeAvailable(cc)

# The compile IR cache records the coretrace-compiler revision that produced each
# entry (InputPipeline.cpp). It is read from the sources FetchContent selected, not
# taken from GIT_TAG above: a parent project's declaration of cc wins over this one.
# It is known only for a clean git checkout that FetchContent made itself, read at
# configure time and again whenever that checkout changes. Sources FetchContent did
# not fetch (FETCHCONTENT_SOURCE_DIR_CC, a SOURCE_DIR, an archive, a package) can
# change without CMake knowing: the revision is then left empty, and the analyzer
# does not use the cache.
set(CORETRACE_COMPILER_REVISION "")
FetchContent_GetProperties(cc)
find_package(Git QUIET)
# FetchContent fetches into "${FETCHCONTENT_BASE_DIR}/cc-src" unless told otherwise.
if(GIT_FOUND AND NOT FETCHCONTENT_SOURCE_DIR_CC
   AND cc_SOURCE_DIR STREQUAL "${FETCHCONTENT_BASE_DIR}/cc-src" AND EXISTS "${cc_SOURCE_DIR}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${cc_SOURCE_DIR}" rev-parse --verify HEAD
        RESULT_VARIABLE cc_head_failed OUTPUT_VARIABLE cc_head
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${cc_SOURCE_DIR}" status --porcelain
        RESULT_VARIABLE cc_status_failed OUTPUT_VARIABLE cc_changes ERROR_QUIET)
    if(NOT cc_head_failed AND NOT cc_status_failed AND cc_changes STREQUAL "")
        set(CORETRACE_COMPILER_REVISION "${cc_head}")
    endif()
    # The revision holds only while the checkout stays as it is: changing any of its files, or
    # its HEAD, makes the next build configure again and read the revision anew.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${cc_SOURCE_DIR}" -c core.quotePath=false ls-files
        RESULT_VARIABLE cc_files_failed OUTPUT_VARIABLE cc_files
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(cc_files_failed)
        set(CORETRACE_COMPILER_REVISION "")
    else()
        string(REPLACE "\n" ";" cc_files "${cc_files}")
        list(TRANSFORM cc_files PREPEND "${cc_SOURCE_DIR}/")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            ${cc_files} "${cc_SOURCE_DIR}/.git/HEAD")
    endif()
endif()
if(CORETRACE_COMPILER_REVISION)
    message(STATUS "coretrace-compiler ${CORETRACE_COMPILER_REVISION}: compile IR cache enabled")
else()
    message(STATUS "coretrace-compiler revision unknown: the compile IR cache is not used")
endif()
