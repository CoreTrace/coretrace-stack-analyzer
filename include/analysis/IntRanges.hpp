// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>
#include <optional>

#include <llvm/ADT/DenseMap.h>

namespace llvm
{
    class BasicBlock;
    class DominatorTree;
    class Function;
    class Value;
} // namespace llvm

namespace ctrace::stack::analysis
{
    struct IntRange
    {
        long long lower = 0;
        long long upper = 0;
        std::uint64_t hasLower : 1 = false;
        std::uint64_t hasUpper : 1 = false;
        std::uint64_t reservedFlags : 62 = 0;
    };

    class FunctionFacts;

    /// @brief Bounds for the integer values of @p F that hold everywhere, keyed by value.
    ///
    /// Only what @p facts can prove at the value's definition (LazyValueInfo, llvm.assume,
    /// KnownBits, ScalarEvolution trip counts), mirrored onto single-assignment slots and
    /// their loads. Branch conditions are deliberately absent: they hold only below the edge
    /// that establishes them, which is what @ref ProgramPointRanges adds.
    std::map<const llvm::Value*, IntRange> computeIntRanges(llvm::Function& F,
                                                            const FunctionFacts& facts);

    /// @brief Bounds for the integer values of @p F at a given basic block.
    ///
    /// Combines @ref computeIntRanges with the constraints of the conditional branches that
    /// dominate the block: for `br (icmp V, C), T, F` the implied bound is recorded on T and
    /// its negation on F whenever that successor has no other predecessor, and a query at
    /// block B intersects the constraints of every such block on B's dominator chain. A
    /// constraint on a `load` is also recorded on the loaded slot, matching how -O0 code
    /// re-reads a variable before each use; a store to that slot between the branch and the
    /// query is not modelled.
    class ProgramPointRanges
    {
      public:
        ProgramPointRanges(llvm::Function& F, const FunctionFacts& facts);

        /// Bounds on @p key that hold throughout @p at, or nullopt if nothing is known.
        [[nodiscard]] std::optional<IntRange> at(const llvm::Value* key,
                                                 const llvm::BasicBlock& at) const;

        /// Every bound that holds throughout @p at, keyed by value.
        [[nodiscard]] std::map<const llvm::Value*, IntRange> at(const llvm::BasicBlock& at) const;

      private:
        using RangeMap = std::map<const llvm::Value*, IntRange>;

        RangeMap proven_;
        llvm::DenseMap<const llvm::BasicBlock*, RangeMap> edgeConstraints_;
        const llvm::DominatorTree& dominators_;
    };
} // namespace ctrace::stack::analysis
