FROM ubuntu:24.04

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
    python3-venv \
    python3 \
    git \
    && rm -rf /var/lib/apt/lists/*

# Install LLVM/Clang toolchain
RUN curl -fsSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh \
    && chmod +x /tmp/llvm.sh \
    && /tmp/llvm.sh ${LLVM_VERSION} \
    && rm -f /tmp/llvm.sh \
    && apt-get update \
    && apt-get install -y --no-install-recommends libclang-20-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /repo
COPY . /repo

RUN rm -rf build \
    && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
       -DLLVM_DIR=/usr/lib/llvm-${LLVM_VERSION}/lib/cmake/llvm \
       -DClang_DIR=/usr/lib/llvm-${LLVM_VERSION}/lib/cmake/clang \
       -DCLANG_LINK_CLANG_DYLIB=ON \
       -DLLVM_LINK_LLVM_DYLIB=ON \
       -DUSE_SHARED_LIB=OFF \
    && cmake --build build -j"$(nproc)"

# RUN bash test/scripts/linux_compile.sh

RUN python3 -m venv .venv \
    && . .venv/bin/activate \
    && python -m pip install --upgrade pip \
    # && python run_test.py
