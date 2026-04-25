# SPDX-License-Identifier: Apache-2.0
include(FetchContent)

# Optional ASAN enablement at the top-level to match the cc dependency.
option(ENABLE_DEBUG_ASAN "Enable debug symbols and AddressSanitizer" OFF)
if(DEFINED DEBUG_ASAN)
    set(ENABLE_DEBUG_ASAN ${DEBUG_ASAN} CACHE BOOL
        "Enable debug symbols and AddressSanitizer" FORCE)
endif()

set(CORETRACE_COMPILER_GIT_TAG "main" CACHE STRING
    "Git ref used when fetching coretrace-compiler")

FetchContent_Declare(
    cc
    GIT_REPOSITORY https://github.com/CoreTrace/coretrace-compiler.git
    GIT_TAG ${CORETRACE_COMPILER_GIT_TAG}
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(cc)
