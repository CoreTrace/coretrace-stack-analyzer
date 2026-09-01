// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>

namespace llvm
{
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

    /// @brief Bounds implied by the integer comparisons appearing anywhere in @p F.
    ///
    /// Flow-insensitive: a constraint is recorded regardless of which branch establishes it.
    /// Prefer @ref computeIntRanges, which narrows this with facts that are actually proven.
    std::map<const llvm::Value*, IntRange> computeIntRangesFromICmps(llvm::Function& F);

    /// @brief Bounds for the integer values of @p F, keyed by value.
    ///
    /// Starts from @ref computeIntRangesFromICmps and intersects every entry with what
    /// @p facts can prove at the value's definition (LazyValueInfo guards, llvm.assume,
    /// KnownBits, ScalarEvolution trip counts), then adds entries for values the comparison
    /// scan says nothing about. Intersection only ever narrows a range, so a consumer using
    /// these bounds to discharge a check can only discharge more of them, never fewer.
    std::map<const llvm::Value*, IntRange> computeIntRanges(llvm::Function& F,
                                                            const FunctionFacts& facts);
} // namespace ctrace::stack::analysis
