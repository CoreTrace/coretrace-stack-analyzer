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
    struct InvalidBaseReconstructionIssue
    {
        std::string funcName;
        std::string varName;        // alloca variable name (stack object)
        std::string sourceMember;   // source member (e.g., "b")
        int64_t offsetUsed = 0;     // offset used in the calculation (can be negative)
        std::string targetType;     // target cast type (e.g., "struct A*")
        bool isOutOfBounds = false; // true if we can prove it is out of bounds
        const llvm::Instruction* inst = nullptr;
    };

    std::vector<InvalidBaseReconstructionIssue> analyzeInvalidBaseReconstructions(
        llvm::Module& mod, const llvm::DataLayout& DL,
        const std::function<bool(const llvm::Function&)>& shouldAnalyze);
} // namespace ctrace::stack::analysis
