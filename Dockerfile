# =============================================================================
# CoreTrace Stack Analyzer — Production Docker Image
# =============================================================================
# Multi-stage build: builds the analyzer, then creates a slim runtime image.
#
# Default runtime behavior (via entrypoint wrapper):
# - auto-detect /workspace/build/compile_commands.json (fallback: /workspace/compile_commands.json)
# - --analysis-profile=fast
# - --compdb-fast
# - --resource-summary-cache-memory-only
# - --resource-model=/models/resource-lifetime/generic.txt
#
# Usage:
#   docker build -t coretrace-stack-analyzer .
#   docker run --rm -v $(pwd):/workspace coretrace-stack-analyzer
#
# Override defaults with explicit args:
#   docker run --rm -v $(pwd):/workspace coretrace-stack-analyzer \
#       --analysis-profile=full --resource-model=/models/resource-lifetime/generic.txt
#
# Bypass defaults completely:
#   docker run --rm -v $(pwd):/workspace coretrace-stack-analyzer --raw --help
# =============================================================================

# ---------------------------------------------------------------------------
# Stage 1: Build
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
ARG LLVM_VERSION=20

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    gnupg \
    lsb-release \
    software-properties-common \
    build-essential \
    cmake \
    ninja-build \
    python3 \
    git \
    && rm -rf /var/lib/apt/lists/*

# Install LLVM/Clang toolchain
RUN curl -fsSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh \
    && chmod +x /tmp/llvm.sh \
    && /tmp/llvm.sh ${LLVM_VERSION} \
    && rm -f /tmp/llvm.sh \
    && apt-get update \
    && apt-get install -y --no-install-recommends libclang-${LLVM_VERSION}-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /repo
COPY . /repo

RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DLLVM_DIR=/usr/lib/llvm-${LLVM_VERSION}/lib/cmake/llvm \
        -DClang_DIR=/usr/lib/llvm-${LLVM_VERSION}/lib/cmake/clang \
        -DCLANG_LINK_CLANG_DYLIB=ON \
        -DLLVM_LINK_LLVM_DYLIB=ON \
        -DUSE_SHARED_LIB=OFF \
    && cmake --build build -j"$(nproc)"

# ---------------------------------------------------------------------------
# Stage 2: Runtime (slim)
# ---------------------------------------------------------------------------
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG LLVM_VERSION=20

# Install only the runtime libraries needed by the analyzer binary
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    gnupg \
    lsb-release \
    software-properties-common \
    python3 \
    git \
    && curl -fsSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh \
    && chmod +x /tmp/llvm.sh \
    && /tmp/llvm.sh ${LLVM_VERSION} \
    && rm -f /tmp/llvm.sh \
    && apt-get update \
    && apt-get install -y --no-install-recommends libclang-${LLVM_VERSION}-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy runtime files
COPY --from=builder /repo/build/stack_usage_analyzer /usr/local/bin/stack_usage_analyzer
COPY --from=builder /repo/scripts/ci/run_code_analysis.py /usr/local/bin/run_code_analysis.py
COPY --from=builder /repo/scripts/docker/coretrace_entrypoint.py /usr/local/bin/coretrace-entrypoint.py
COPY --from=builder /repo/models /models

RUN chmod +x /usr/local/bin/coretrace-entrypoint.py

# Make sure the binary can find LLVM shared libs
ENV LD_LIBRARY_PATH=/usr/lib/llvm-${LLVM_VERSION}/lib

WORKDIR /workspace

ENTRYPOINT ["/usr/local/bin/coretrace-entrypoint.py"]
