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
# The compile IR cache records it with each entry (InputPipeline.cpp).
set(CORETRACE_COMPILER_REVISION b4ad33638e9020a93f052a7a299e4105bd48f975) # v0.10.0
FetchContent_Declare(
    cc
    GIT_REPOSITORY https://github.com/CoreTrace/coretrace-compiler.git
    GIT_TAG ${CORETRACE_COMPILER_REVISION}
)
FetchContent_MakeAvailable(cc)
