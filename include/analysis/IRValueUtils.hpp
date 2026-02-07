#pragma once

#include <string>

namespace llvm
{
    class AllocaInst;
    class ConstantInt;
    class Function;
    class Value;
} // namespace llvm

namespace ctrace::stack::analysis
{
    std::string deriveAllocaName(const llvm::AllocaInst* AI);

    const llvm::ConstantInt* tryGetConstFromValue(const llvm::Value* V,
                                                  const llvm::Function& F);
} // namespace ctrace::stack::analysis
