#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Builds the analyzer of /work/<variant> as the CI does on Linux (ci.yml), unit tests included.
#   build.sh <variant>
#   build.sh rework no-release    the rework of 272 again, without its release of freed memory
set -euo pipefail

variant=$1
cd "/work/${variant}"
start=$(date +%s)
if [[ "${2:-}" == no-release ]]; then
    # One line goes away: the call that gives the freed memory back after loading.
    cp src/app/AnalyzerApp.cpp /work/AnalyzerApp.cpp.with-release
    sed -i '/^    releaseFreedMemory();$/d' src/app/AnalyzerApp.cpp
    if cmp -s src/app/AnalyzerApp.cpp /work/AnalyzerApp.cpp.with-release; then
        echo "the call to releaseFreedMemory was not found" >&2
        exit 1
    fi
else
    cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DLLVM_DIR=/usr/lib/llvm-20/lib/cmake/llvm \
        -DClang_DIR=/usr/lib/llvm-20/lib/cmake/clang \
        -DUSE_SHARED_LIB=OFF \
        -DBUILD_TESTS=OFF \
        -DBUILD_ANALYZER_UNIT_TESTS=ON \
        -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld
fi
cmake --build build -j4
echo "built ${variant} ${2:-} in $(($(date +%s) - start)) s"
