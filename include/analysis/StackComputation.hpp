#pragma once

#include <map>
#include <set>
#include <vector>

#include "StackUsageAnalyzer.hpp"

namespace llvm
{
    class DataLayout;
    class Function;
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    using CallGraph = std::map<const llvm::Function*, std::vector<const llvm::Function*>>;

    struct StackEstimate
    {
        StackSize bytes = 0;
        bool unknown = false;
    };

    struct LocalStackInfo
    {
        StackSize bytes = 0;
        bool unknown = false;
        bool hasDynamicAlloca = false;
        std::vector<std::pair<std::string, StackSize>> localAllocas;
    };

    struct InternalAnalysisState
    {
        std::map<const llvm::Function*, StackEstimate> TotalStack; // max stack, including callees
        std::set<const llvm::Function*> RecursiveFuncs;         // functions in at least one cycle
        std::set<const llvm::Function*> InfiniteRecursionFuncs; // “infinite” self-recursion
    };

    CallGraph buildCallGraph(llvm::Module& M);

    LocalStackInfo computeLocalStack(llvm::Function& F, const llvm::DataLayout& DL,
                                     AnalysisMode mode);

    InternalAnalysisState
    computeGlobalStackUsage(const CallGraph& CG,
                            const std::map<const llvm::Function*, LocalStackInfo>& LocalStack);

    bool detectInfiniteSelfRecursion(llvm::Function& F);

    StackSize computeAllocaLargeThreshold(const AnalysisConfig& config);
} // namespace ctrace::stack::analysis
