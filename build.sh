mkdir -p build && cd build

cmake .. \
  -DLLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm \
  -DClang_DIR=$(brew --prefix llvm)/lib/cmake/clang \
  -DCMAKE_BUILD_TYPE=Release \
&& make -j$(sysctl -n hw.logicalcpu)
