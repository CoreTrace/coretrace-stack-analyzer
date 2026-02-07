#pragma once

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

        bool userControlled = false;      // size derived from argument / non-local value
        bool sizeIsConst = false;         // size known exactly
        bool hasUpperBound = false;       // bounded size (from ICmp-derived range)
        bool isRecursive = false;         // function participates in a recursion cycle
        bool isInfiniteRecursive = false; // unconditional self recursion

        StackSize sizeBytes = 0;       // exact size in bytes (if sizeIsConst)
        StackSize upperBoundBytes = 0; // upper bound in bytes (if hasUpperBound)
    };

    std::vector<AllocaUsageIssue> analyzeAllocaUsage(
        llvm::Module& mod,
        const llvm::DataLayout& DL,
        const std::set<const llvm::Function*>& recursiveFuncs,
        const std::set<const llvm::Function*>& infiniteRecursionFuncs,
        const std::function<bool(const llvm::Function&)>& shouldAnalyze);
} // namespace ctrace::stack::analysis
