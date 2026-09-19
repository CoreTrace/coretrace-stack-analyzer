// SPDX-License-Identifier: Apache-2.0
// Forward dataflow over OwnershipFacts. LLVM-free; the single owner of the
// transfer semantics (applyEvent) used both to solve and to replay.
#pragma once

#include "analysis/ownership/OwnershipDomain.hpp"
#include "analysis/ownership/OwnershipFacts.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace ctrace::stack::analysis::ownership
{
    struct ExitRecord
    {
        AbstractState state;
        std::uint32_t block = 0;
        std::uint32_t eventIndex = 0;
        bool exceptional = false;
        std::uint8_t reservedPadding[7] = {};
    };

    struct OwnershipResult
    {
        std::vector<AbstractState> in;  // by block
        std::vector<AbstractState> out; // by block
        std::vector<ExitRecord> exits;  // in block/event order; empty when incomplete
        bool incomplete = false;        // budget exhausted: nothing below may be trusted
        std::uint8_t reservedPadding[7] = {};
    };

    /// Applies one event to a state. transfer(⊥) = ⊥.
    void applyEvent(const OwnershipFacts& facts, const Event& e, AbstractState& s);

    /// Iterates to a fixpoint from `entry` at block 0. `iterationLimit` 0 picks
    /// max(64, 16 × blocks).
    OwnershipResult solve(const OwnershipFacts& facts, const AbstractState& entry,
                          unsigned iterationLimit = 0);

    /// Replays the events of `block` from result.in[block]; cb(eventIndex, before, after).
    void replay(
        const OwnershipFacts& facts, const OwnershipResult& result, std::uint32_t block,
        const std::function<void(std::uint32_t, const AbstractState&, const AbstractState&)>& cb);
} // namespace ctrace::stack::analysis::ownership
