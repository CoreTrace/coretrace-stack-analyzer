#pragma once

#include <functional>
#include <string>
#include <vector>

#include "StackUsageAnalyzer.hpp"

namespace llvm
{
    class DataLayout;
    class Function;
    class Instruction;
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    struct MemIntrinsicIssue
    {
        std::string funcName;
        std::string varName;
        std::string intrinsicName; // "memcpy" / "memset" / "memmove"
        StackSize destSizeBytes = 0;
        StackSize lengthBytes = 0;
        const llvm::Instruction* inst = nullptr;
    };

    std::vector<MemIntrinsicIssue>
    analyzeMemIntrinsicOverflows(llvm::Module& mod,
                                 const llvm::DataLayout& DL,
                                 const std::function<bool(const llvm::Function&)>& shouldAnalyze);
} // namespace ctrace::stack::analysis
