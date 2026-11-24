// SPDX-License-Identifier: Apache-2.0
// Forward dataflow over OwnershipFacts. LLVM-free; the single owner of the
// transfer semantics (applyEvent) used both to solve and to replay.
#pragma once

#include "analysis/ownership/OwnershipDomain.hpp"
#include "analysis/ownership/OwnershipFacts.hpp"

#include <cstddef>
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
        std::uint64_t blockVisits = 0;  // blocks the worklist processed; observation only
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

    /// A Use event that may read a released resource.
    struct ReleasedUse
    {
        const Event* use = nullptr; // points into the facts
        ResourceId resource = 0;
        std::uint32_t reservedPadding = 0;
    };

    /// Every (Use, resource) where the resource the Use's `src` may hold may be Released and is
    /// not uncertain, or is Released on every path where it exists (uncertain or not), read off
    /// the stabilised states (edges included). Empty when incomplete.
    std::vector<ReleasedUse> releasedUses(const OwnershipFacts& facts,
                                          const OwnershipResult& result);

    /// What the engine can tell about one release (DoubleRelease).
    enum class ReleaseOutcome : std::uint8_t
    {
        Unknown,  // the engine cannot decide: the caller keeps its own checks
        Checked,  // everything the released location may hold is null or a resource still owned
        Detected, // the location may hold a resource released before: a potential double release
    };

    /// The verdict on the location released at one instruction, by a Release event or by a
    /// parameter of a Call event that releases an owned resource without uncertainty.
    struct ReleaseVerdict
    {
        std::uint32_t instructionIndex = 0;
        LocationId location = 0;
        ResourceId resource = 0; // Detected: one of the released resources the location holds
        ReleaseOutcome outcome = ReleaseOutcome::Unknown;
        std::uint8_t reservedPadding[3] = {};
    };

    /// Every release reached in the stabilised states (edges included), in block/event order,
    /// judged on the state just before it:
    ///  - one possible resource and nothing else: Detected when it may be Released (the
    ///    releasedUses filter), Checked when it is only Owned and certain;
    ///  - several possibilities (resources, null, unknown): Detected when each resource is
    ///    Released on every path where it exists, Checked when each is only Owned and certain
    ///    and nothing is unknown;
    ///  - null only: Checked; unknown only or nothing tracked: Unknown.
    /// The parameters of one Call are all judged on the state before the call: argument order is
    /// not execution order. When two of them hold exactly the same single resource and both
    /// release it on every normal exit, the later one in argument order is Detected. When their
    /// targets merely overlap, or the other one may release only on some exits, a Checked
    /// verdict becomes Unknown. Copies of one release (the edges of an invoke) must agree, else
    /// Unknown. Empty when incomplete. Meant to be read off withUnknownAtEntry(facts).
    std::vector<ReleaseVerdict> judgeReleases(const OwnershipFacts& facts,
                                              const OwnershipResult& result);

    /// `facts` with "unknown" written at entry into every location that may hold a value the
    /// engine does not follow: every storage location (`isSlot[loc]`, or a location that is not
    /// Local) and every SSA value that no event writes (the result of an unknown call, an
    /// argument, untracked memory). Such a location is otherwise empty, and a join forgets an
    /// empty path: joined with a null path it would look "null only". Transitional: only the
    /// DoubleRelease verdicts solve these facts; the other rules still read an empty location as
    /// holding nothing.
    OwnershipFacts withUnknownAtEntry(const OwnershipFacts& facts, const std::vector<bool>& isSlot);

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
    /// The work of one computeSummary, for measurement only: it never changes the summary.
    struct SummaryWork
    {
        std::uint64_t blockVisits = 0;
        std::uint32_t solves = 0;
        std::uint32_t reservedPadding = 0;
    };

    FunctionOwnershipSummary computeSummary(const OwnershipFacts& facts,
                                            unsigned iterationLimit = 0,
                                            SummaryWork* work = nullptr);
    /// Whether two summaries are the same, incompleteness included.
    bool sameSummary(const FunctionOwnershipSummary& a, const FunctionOwnershipSummary& b);

    /// The work of one computeSummaryFixpoint, for measurement only: it never changes a summary.
    struct SummaryFixpointWork
    {
        std::uint64_t computations = 0; // summaries computed
        std::uint64_t reused = 0;       // summaries kept: none of those they consulted changed
        std::uint32_t rounds = 0;
        std::uint32_t reservedPadding = 0;
    };

    using ConsultSummary = std::function<const FunctionOwnershipSummary&(std::size_t)>;

    /// The summaries of count functions that may consult each other's. Round by round, in index
    /// order, compute(i, consult) gives the summary of function i, reading the current summary of
    /// function j through consult(j), until a round changes none; past maxRounds rounds, every
    /// summary is incomplete. Every summary starts empty. compute must read the summaries of these
    /// functions through consult only, and depend on nothing else that changes.
    std::vector<FunctionOwnershipSummary> computeSummaryFixpoint(
        std::size_t count,
        const std::function<FunctionOwnershipSummary(std::size_t, const ConsultSummary&)>& compute,
        unsigned maxRounds, SummaryFixpointWork* work = nullptr);
} // namespace ctrace::stack::analysis::ownership
