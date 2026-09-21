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
        std::uint32_t block = 0;      // the block, or the edge's source block
        std::uint32_t eventIndex = 0; // index in the block's or the edge's events
        std::uint32_t edge = kNoEdge; // set when the Exit event lives on an edge
        bool exceptional = false;
        std::uint8_t reservedPadding[3] = {};

        static constexpr std::uint32_t kNoEdge = ~0u;
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

    /// Union of t[s] over the states s in `in`.
    StateSet applyTransformer(const ParamTransformer& t, StateSet in);

    /// The transformer of "first, then `then`".
    ParamTransformer composeTransformers(const ParamTransformer& first,
                                         const ParamTransformer& then);

    /// Pointwise union.
    void joinTransformer(ParamTransformer& into, const ParamTransformer& other);

    /// Summarises a function as transformers of its parameters, per exit kind: for each
    /// parameter location of `facts.paramLocations` and each singleton input state, one
    /// solve with that state; the image is the join of the parameter's resource state over
    /// the exits of each kind. Fresh resources: `returns` is Guaranteed when every normal
    /// exit returns exactly a fresh resource, Conditional when some do, Unknown otherwise
    /// (likewise `outArgs` for ArgPointee locations).
    FunctionOwnershipSummary computeSummary(const OwnershipFacts& facts,
                                            unsigned iterationLimit = 0);
} // namespace ctrace::stack::analysis::ownership
