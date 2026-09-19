// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <utility>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>

namespace llvm
{
    class AllocaInst;
    class BasicBlock;
    class DominatorTree;
    class Function;
    class Instruction;
    class StoreInst;
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

    /// @brief Bounds for the integer values of @p F at a given instruction.
    ///
    /// Combines @ref computeIntRanges with the constraints of the conditional branches that
    /// dominate the point: for `br (icmp V, C), T, F` the implied bound is recorded on T and
    /// its negation on F whenever that successor has no other predecessor, and a query at
    /// instruction I intersects the constraints of every such block on I's dominator chain.
    ///
    /// A constraint on a `load` is also recorded on the loaded slot, matching how -O0 code
    /// re-reads a variable before each use. That slot constraint is dropped at I when a
    /// store to the slot can reach I without re-entering the block that established it
    /// (a loop's own increment re-evaluates the guard; `i = 1` after the loop does not).
    /// Slots whose address is taken carry no constraints at all.
    class ProgramPointRanges
    {
      public:
        ProgramPointRanges(llvm::Function& F, const FunctionFacts& facts);

        /// Bounds on @p key that hold at @p at, or nullopt if nothing is known.
        [[nodiscard]] std::optional<IntRange> at(const llvm::Value* key,
                                                 const llvm::Instruction& at) const;

        /// Every bound that holds at @p at, keyed by value.
        [[nodiscard]] std::map<const llvm::Value*, IntRange> at(const llvm::Instruction& at) const;

      private:
        using RangeMap = std::map<const llvm::Value*, IntRange>;
        using BlockSet = llvm::DenseSet<const llvm::BasicBlock*>;

        [[nodiscard]] bool constraintKilledAt(const llvm::BasicBlock& establishing,
                                              const llvm::Value* key,
                                              const llvm::Instruction& at) const;
        [[nodiscard]] const BlockSet&
        blocksAfterStoresAvoiding(const llvm::BasicBlock& establishing,
                                  const llvm::AllocaInst& slot) const;

        RangeMap proven_;
        llvm::DenseMap<const llvm::BasicBlock*, RangeMap> edgeConstraints_;
        llvm::DenseMap<const llvm::AllocaInst*, llvm::SmallVector<const llvm::StoreInst*, 4>>
            slotStores_;
        mutable llvm::DenseMap<std::pair<const llvm::BasicBlock*, const llvm::AllocaInst*>,
                               BlockSet>
            afterStoresCache_;
        const llvm::DominatorTree& dominators_;
    };
} // namespace ctrace::stack::analysis
