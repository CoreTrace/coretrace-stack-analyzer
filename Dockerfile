# =============================================================================
# CoreTrace Stack Analyzer — Docker Image
# =============================================================================
# This Dockerfile supports 3 user-facing targets:
#   - dev:
#       toolchain + repo checkout, no build; default command is an interactive shell.
#       Use it to run cmake/build/tests manually.
#   - builder:
#       compiles the analyzer into /repo/build/stack_usage_analyzer.
#       Use it in CI or to extract binaries/artifacts.
#   - runtime:
#       production image with analyzer + models + Docker entrypoint wrapper.
#       Default workdir is /workspace and entrypoint auto-resolves compile_commands.json.
#
# Typical commands:
#   # 1) Dev mode (interactive)
#   docker build --target dev -t coretrace-stack-analyzer:dev .
#   docker run --rm -it -v "$PWD:/repo" -w /repo coretrace-stack-analyzer:dev
#
#   # 2) Builder mode (compile artifacts)
#   docker build --target builder -t coretrace-stack-analyzer:builder .
#   docker create --name coretrace-builder coretrace-stack-analyzer:builder
#   docker cp coretrace-builder:/repo/build/stack_usage_analyzer ./build/stack_usage_analyzer
#   docker rm coretrace-builder
#
#   # 3) Runtime mode (analyze project from compile_commands.json)
#   docker build --target runtime -t coretrace-stack-analyzer:runtime .
#   docker run --rm -v "$PWD:/workspace" coretrace-stack-analyzer:runtime
#   # pass --raw to bypass wrapper defaults:
#   docker run --rm -v "$PWD:/workspace" coretrace-stack-analyzer:runtime --raw --help
# =============================================================================

# ---------------------------------------------------------------------------
# Stage 0: Base (toolchain + build deps)
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS base

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

# Make sure LLVM shared libs are found at runtime (useful for dev builds too)
ENV LD_LIBRARY_PATH=/usr/lib/llvm-${LLVM_VERSION}/lib

# ---------------------------------------------------------------------------
# Stage 1: Dev (deps + repo, no build)
# ---------------------------------------------------------------------------
FROM base AS dev

WORKDIR /repo
COPY . /repo

# Default: interactive shell so you can build/test manually
CMD ["bash"]

# ---------------------------------------------------------------------------
# Stage 2: Build (produces binaries)
# ---------------------------------------------------------------------------
FROM base AS builder

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
# Stage 3: Runtime (prod)
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

ARG DEBIAN_FRONTEND=noninteractive
ARG LLVM_VERSION=20

# Install only what is needed to run (and to support the entrypoint script)
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

ENV LD_LIBRARY_PATH=/usr/lib/llvm-${LLVM_VERSION}/lib

WORKDIR /workspace

ENTRYPOINT ["/usr/local/bin/coretrace-entrypoint.py"]
