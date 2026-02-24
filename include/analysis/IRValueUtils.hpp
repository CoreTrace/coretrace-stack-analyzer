#pragma once

#include <string>
#include <llvm/ADT/StringRef.h>

namespace llvm
{
    class AllocaInst;
    class ConstantInt;
    class Function;
    class Value;
} // namespace llvm

namespace ctrace::stack::analysis
{
    enum class AllocaOrigin
    {
        User,
        CompilerGenerated,
        Unknown
    };

    std::string deriveAllocaName(const llvm::AllocaInst* AI);

    bool isLikelyCompilerTemporaryName(llvm::StringRef name);

    AllocaOrigin classifyAllocaOrigin(const llvm::AllocaInst* AI);

    const llvm::ConstantInt* tryGetConstFromValue(const llvm::Value* V, const llvm::Function& F);
} // namespace ctrace::stack::analysis
