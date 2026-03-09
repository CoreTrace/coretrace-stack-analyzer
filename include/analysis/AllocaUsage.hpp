#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "StackUsageAnalyzer.hpp"

namespace llvm
{
    class AllocaInst;
    class DataLayout;
    class Function;
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    struct AllocaUsageIssue
    {
        std::string funcName;
        std::string varName;
        const llvm::AllocaInst* allocaInst = nullptr;

        StackSize sizeBytes = 0;       // exact size in bytes (if sizeIsConst)
        StackSize upperBoundBytes = 0; // upper bound in bytes (if hasUpperBound)

        std::uint64_t userControlled : 1 = false; // size derived from argument / non-local value
        std::uint64_t sizeIsConst : 1 = false;    // size known exactly
        std::uint64_t hasUpperBound : 1 = false;  // bounded size (from ICmp-derived range)
        std::uint64_t isRecursive : 1 = false;    // function participates in a recursion cycle
        std::uint64_t isInfiniteRecursive : 1 = false; // unconditional self recursion
        std::uint64_t reservedFlags : 59 = 0;
    };

    std::vector<AllocaUsageIssue>
    analyzeAllocaUsage(llvm::Module& mod, const llvm::DataLayout& DL,
                       const std::set<const llvm::Function*>& recursiveFuncs,
                       const std::set<const llvm::Function*>& infiniteRecursionFuncs,
                       const std::function<bool(const llvm::Function&)>& shouldAnalyze);
} // namespace ctrace::stack::analysis
