#pragma once

#include <functional>
#include <string>
#include <vector>

namespace llvm
{
    class Function;
    class Instruction;
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    enum class NullDerefIssueKind
    {
        DirectNullPointer,
        NullBranchDereference,
        NullStoredInLocalSlot,
        UncheckedAllocatorResult
    };

    struct NullDerefIssue
    {
        std::string funcName;
        std::string filePath;
        std::string pointerName;
        NullDerefIssueKind kind = NullDerefIssueKind::DirectNullPointer;
        const llvm::Instruction* inst = nullptr;
    };

    std::vector<NullDerefIssue>
    analyzeNullDereferences(llvm::Module& mod,
                            const std::function<bool(const llvm::Function&)>& shouldAnalyze);
} // namespace ctrace::stack::analysis
