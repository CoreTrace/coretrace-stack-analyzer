// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
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
    enum class NullDerefIssueKind : std::uint64_t
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
        const llvm::Instruction* inst = nullptr;
        NullDerefIssueKind kind = NullDerefIssueKind::DirectNullPointer;
    };

    std::vector<NullDerefIssue>
    analyzeNullDereferences(llvm::Module& mod,
                            const std::function<bool(const llvm::Function&)>& shouldAnalyze);
} // namespace ctrace::stack::analysis
