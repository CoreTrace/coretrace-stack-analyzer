// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "StackUsageAnalyzer.hpp"

namespace llvm
{
    class CallBase;
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

    // The definition a call reaches among the modules analyzed together (#157): the one its own
    // module has, or, for a declaration, the only definition of the same linker symbol, when it
    // is exact. A static function is never reached from another module. A call through a
    // declaration without a prototype has another type than its callee, and still reaches it.
    class CallResolver
    {
      public:
        explicit CallResolver(const std::vector<llvm::Module*>& modules);
        const llvm::Function* resolve(const llvm::CallBase& call) const;

      private:
        std::unordered_map<std::string, const llvm::Function*> definitions_;
    };

    CallGraph buildCallGraph(llvm::Module& M);

    // A call that the resolver resolves counts as a call to a definition of the same module.
    LocalStackInfo computeLocalStack(llvm::Function& F, const llvm::DataLayout& DL,
                                     AnalysisMode mode, const CallResolver* resolver = nullptr);

    // Unresolved calls make the max stack unknown unless
    // config.assumeExternalFrame is set, in which case each one is charged
    // config.assumeExternalFrameBytes as its callee subtree.
    //
    // A function in a call cycle, or one that calls into a cycle, has an unknown max stack: the
    // depth of the recursion is not bounded. Its lower bound depends on the call graph and the
    // frames only, not on @p Order, the functions of LocalStack in the module's order.
    InternalAnalysisState computeGlobalStackUsage(
        const CallGraph& CG, const std::map<const llvm::Function*, LocalStackInfo>& LocalStack,
        const std::vector<const llvm::Function*>& Order, const AnalysisConfig& config);

    std::vector<std::vector<const llvm::Function*>>
    computeRecursiveComponents(const CallGraph& CG,
                               const std::vector<const llvm::Function*>& nodes);

    bool detectInfiniteSelfRecursion(llvm::Function& F);
    bool detectInfiniteSelfRecursion(llvm::Function& F, const AnalysisConfig& config);
    bool detectInfiniteRecursionComponent(const std::vector<const llvm::Function*>& component);
    // A call is recursive when it reaches a member of the component, through the resolver when
    // set (#157).
    bool detectInfiniteRecursionComponent(const std::vector<const llvm::Function*>& component,
                                          const AnalysisConfig& config,
                                          const CallResolver* resolver = nullptr);

    // The max stack, the recursive functions and the infinite recursions of the functions of
    // @p Order, on the call graph @p CG.
    InternalAnalysisState
    computeStackState(const CallGraph& CG,
                      const std::map<const llvm::Function*, LocalStackInfo>& LocalStack,
                      const std::vector<const llvm::Function*>& Order, const AnalysisConfig& config,
                      const CallResolver* resolver = nullptr);

    // One call graph over the modules analyzed together (#157): the calls within each module,
    // and those that the resolver resolves into another one. The local stacks and the state of
    // every defined function are computed on it, as for one module.
    struct GlobalStackFacts
    {
        CallGraph graph;
        std::map<const llvm::Function*, LocalStackInfo> localStack;
        InternalAnalysisState state;
    };

    GlobalStackFacts computeGlobalStackFacts(const std::vector<llvm::Module*>& modules,
                                             const AnalysisConfig& config);

    StackSize computeAllocaLargeThreshold(const AnalysisConfig& config);
} // namespace ctrace::stack::analysis
