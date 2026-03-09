#pragma once

#include <cstdint>
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
        std::string intrinsicName;
        StackSize destSizeBytes = 0;
        StackSize lengthBytes = 0;
        const llvm::Instruction* inst = nullptr;
        std::uint64_t hasExplicitLength : 1 = false;
        std::uint64_t reservedFlags : 63 = 0;
    };

    std::vector<MemIntrinsicIssue>
    analyzeMemIntrinsicOverflows(llvm::Module& mod, const llvm::DataLayout& DL,
                                 const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                                 const std::string& bufferModelPath = "");
} // namespace ctrace::stack::analysis
