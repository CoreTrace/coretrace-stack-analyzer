// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "analysis/IntRanges.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace llvm
{
    class DominatorTree;
    class Function;
    class Instruction;
    class LazyValueInfo;
    class TargetLibraryInfo;
    class Value;
} // namespace llvm

namespace ctrace::stack::analysis
{
    /// @brief Per-function bundle of upstream LLVM analyses, built once and shared.
    ///
    /// LazyValueInfo needs an AssumptionCache, ObjectSizeOffsetVisitor needs a
    /// TargetLibraryInfo, and both want a DominatorTree. Owning them together keeps that
    /// wiring in one place instead of re-deriving it in every analysis that wants a fact
    /// about a value.
    ///
    /// ScalarEvolution is deliberately absent. The analyzer reads -O0 IR, where a loop
    /// counter lives in a multiply-assigned alloca that SCEV cannot see through, and the
    /// arithmetic it would bound carries nsw, which @ref computeIntRanges refuses on purpose.
    /// Wiring it in changed no result on any fixture; add it back only alongside a pass that
    /// promotes locals to registers.
    class FunctionFacts final
    {
      public:
        explicit FunctionFacts(llvm::Function& function);
        ~FunctionFacts();

        FunctionFacts(const FunctionFacts&) = delete;
        FunctionFacts& operator=(const FunctionFacts&) = delete;
        FunctionFacts(FunctionFacts&&) = delete;
        FunctionFacts& operator=(FunctionFacts&&) = delete;

        [[nodiscard]] const llvm::DominatorTree& dominatorTree() const;
        [[nodiscard]] llvm::LazyValueInfo& lazyValueInfo() const;
        [[nodiscard]] const llvm::TargetLibraryInfo& targetLibraryInfo() const;

        /// @brief Signed range of @p value provable at program point @p at.
        /// @param value Integer value to bound; non-integer values yield no range.
        /// @param at Context instruction, or nullptr to only use context-free facts.
        /// @return The range, or nullopt when nothing narrower than the full type is known.
        [[nodiscard]] std::optional<IntRange> signedRangeAt(const llvm::Value* value,
                                                            const llvm::Instruction* at) const;

        /// @brief Signed range of @p value provable wherever it is live.
        ///
        /// Queried at the definition of @p value: the set of values a definition can take is
        /// fixed there, and dominating guards can only narrow it further downstream, so the
        /// result stays valid at every use. This is what makes the range safe to publish in a
        /// program-point-free map such as the one returned by @ref computeIntRanges.
        [[nodiscard]] std::optional<IntRange> signedRange(const llvm::Value* value) const;

        /// @brief Allocation size, in bytes, of the object @p pointer points into.
        ///
        /// Backed by ObjectSizeOffsetVisitor, so it covers allocas, globals, byval arguments
        /// and TargetLibraryInfo-recognised allocators (malloc/calloc/realloc/new/...).
        /// Merge points keep the smallest candidate, so the size holds on every path.
        [[nodiscard]] std::optional<std::uint64_t>
        objectSizeBytes(const llvm::Value* pointer) const;

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace ctrace::stack::analysis
