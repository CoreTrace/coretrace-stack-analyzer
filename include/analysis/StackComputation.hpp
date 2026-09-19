// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
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
        std::uint64_t unknown : 1 = false;
        std::uint64_t reservedFlags : 63 = 0;
    };

    struct LocalStackInfo
    {
        StackSize bytes = 0;
        std::vector<std::pair<std::string, StackSize>> localAllocas;
        // Calls whose callee frame cannot be computed from this module: indirect
        // calls and calls to declarations without a definition. Intrinsics excluded.
        std::uint64_t unresolvedCallCount = 0;
        std::uint64_t unknown : 1 = false;
        std::uint64_t hasDynamicAlloca : 1 = false;
        std::uint64_t reservedFlags : 62 = 0;
    };

    struct InternalAnalysisState
    {
        std::map<const llvm::Function*, StackEstimate> TotalStack; // max stack, including callees
        std::set<const llvm::Function*> RecursiveFuncs;         // functions in at least one cycle
        std::set<const llvm::Function*> InfiniteRecursionFuncs; // recursive cycle with no base case
    };

    CallGraph buildCallGraph(llvm::Module& M);

    LocalStackInfo computeLocalStack(llvm::Function& F, const llvm::DataLayout& DL,
                                     AnalysisMode mode);

    // Unresolved calls make the max stack unknown unless
    // config.assumeExternalFrame is set, in which case each one is charged
    // config.assumeExternalFrameBytes as its callee subtree.
    InternalAnalysisState
    computeGlobalStackUsage(const CallGraph& CG,
                            const std::map<const llvm::Function*, LocalStackInfo>& LocalStack,
                            const AnalysisConfig& config);

    std::vector<std::vector<const llvm::Function*>>
    computeRecursiveComponents(const CallGraph& CG,
                               const std::vector<const llvm::Function*>& nodes);

    bool detectInfiniteSelfRecursion(llvm::Function& F);
    bool detectInfiniteSelfRecursion(llvm::Function& F, const AnalysisConfig& config);
    bool detectInfiniteRecursionComponent(const std::vector<const llvm::Function*>& component);
    bool detectInfiniteRecursionComponent(const std::vector<const llvm::Function*>& component,
                                          const AnalysisConfig& config);

    StackSize computeAllocaLargeThreshold(const AnalysisConfig& config);
} // namespace ctrace::stack::analysis
