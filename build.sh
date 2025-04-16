mkdir -p build && cd build

cmake .. \
  -DLLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm \
  -DCMAKE_BUILD_TYPE=Release \
&& make -j$(sysctl -n hw.logicalcpu)
