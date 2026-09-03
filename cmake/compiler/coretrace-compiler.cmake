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
    GIT_TAG 866fa76403f29e4fefda99770e04d284175f4408 # v0.7.0-33-g866fa76
)
FetchContent_MakeAvailable(cc)
