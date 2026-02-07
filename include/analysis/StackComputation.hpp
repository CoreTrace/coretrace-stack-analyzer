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
        std::map<const llvm::Function*, StackEstimate> TotalStack; // stack max, callees inclus
        std::set<const llvm::Function*> RecursiveFuncs;         // fonctions dans au moins un cycle
        std::set<const llvm::Function*> InfiniteRecursionFuncs; // auto-récursion “infinie”
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
