// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace llvm
{
    class Function;
    class Instruction;
    class Module;
    class Value;
} // namespace llvm

namespace ctrace::stack::analysis
{
    /// @brief The values an index takes where its access runs.
    struct IndexValues
    {
        enum class Kind : std::uint32_t
        {
            Unknown,   ///< Nothing is established, or the case is outside the conditions.
            NeverRuns, ///< The access never runs: a loop around it runs no iteration.
            Range,     ///< Every value the index takes where the access runs is in [min, max].
        };

        std::int64_t min = 0;
        std::int64_t max = 0;
        Kind kind = Kind::Unknown;
        /// Range only: on every run of the function that returns, the access runs with the
        /// index equal to this endpoint.
        std::uint32_t minOnEveryRun : 1 = false;
        std::uint32_t maxOnEveryRun : 1 = false;
        std::uint32_t reservedFlags : 30 = 0;
    };

    /// @brief Value queries about the original IR, answered on a copy of the module where
    /// promotable locals live in registers, so ScalarEvolution sees loop counters.
    ///
    /// The original IR stays the input of every analysis: the copy only answers questions about
    /// the values original instructions compute. It is sanitized so that its facts hold for the
    /// original program:
    /// - each promotable slot starts with an opaque value, so a read that may be uninitialized
    ///   stays unknown instead of taking the value of a later store;
    /// - the wrap and poison flags (nsw, nuw, exact, inbounds, nneg, disjoint, samesign),
    ///   optnone, the mustprogress and willreturn attributes and the loop metadata are dropped,
    ///   so no fact rests on undefined behavior or on a forward-progress promise;
    /// - functions with lifetime markers are left out.
    ///
    /// Index ranges use the endpoint method: they cover affine recurrences whose no-wrap
    /// ScalarEvolution proves, in loops that exit only from their header, with every input
    /// bounded by the program rather than by its type. Anything else is Kind::Unknown.
    class PromotedValues final
    {
      public:
        /// Copies the declarations of @p module. A function @p shouldCopy accepts is copied and
        /// promoted on its first query; only the last function queried is kept.
        PromotedValues(const llvm::Module& module,
                       const std::function<bool(const llvm::Function&)>& shouldCopy);
        ~PromotedValues();

        PromotedValues(const PromotedValues&) = delete;
        PromotedValues& operator=(const PromotedValues&) = delete;

        /// Values @p index takes where @p access runs. Both belong to the original IR. Pass the
        /// load or store itself, not the address it uses: the conditions it runs on count.
        [[nodiscard]] IndexValues indexValuesAt(const llvm::Instruction& access,
                                                const llvm::Value& index);

        /// Whether ScalarEvolution proves @p lower <= @p index < @p upper wherever @p access
        /// runs, reading the index as signed.
        [[nodiscard]] bool provesWithin(const llvm::Instruction& access, const llvm::Value& index,
                                        std::int64_t lower, std::int64_t upper);

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace ctrace::stack::analysis
