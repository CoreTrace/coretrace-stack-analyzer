// SPDX-License-Identifier: Apache-2.0
#include "analysis/ownership/OwnershipEngine.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <string>

namespace ctrace::stack::analysis::ownership
{
    namespace
    {

        /// Marks every resource `src` may hold with `add`; strong (replace) only when the
        /// contents name exactly one resource and the effect is guaranteed.
        void markHeld(AbstractState& s, const Contents& held, StateSet add, bool guaranteed)
        {
            for (const ResourceId r : held.resources)
            {
                // Transferring or releasing a conditional resource cannot make it exist
                // on the paths where its acquisition failed.
                if (s.resources[r].isOnly(OwnState::NotOwned))
                    continue;
                StateSet next = add;
                if (s.resources[r].has(OwnState::NotOwned))
                    next |= StateSet::of(OwnState::NotOwned);
                if (held.isExactly(r) && guaranteed)
                    s.resources[r] = next;
                else
                    s.resources[r] |= next;
            }
        }

        void retargetLocations(AbstractState& s, ResourceId from, ResourceId to)
        {
            for (Contents& c : s.locations)
            {
                if (!c.holds(from))
                    continue;
                c.remove(from);
                c.add(to);
            }
        }

        void acquireInto(const OwnershipFacts& facts, AbstractState& s, std::uint32_t site,
                         LocationId dst, bool strong, Certainty certainty)
        {
            const ResourceId rn = newInstanceOf(site);
            const ResourceId ro = oldInstancesOf(site);
            // The previous instance, if any, becomes one of the "older" ones. "No older
            // instance" (NotOwned on `ro`) is now true exactly when there was no previous
            // instance, which `rn` says with its own NotOwned.
            StateSet older = s.resources[ro];
            older.bits &= static_cast<std::uint8_t>(~StateSet::of(OwnState::NotOwned).bits);
            s.resources[ro] = older | s.resources[rn];
            s.uncertain[ro] = s.uncertain[ro] || s.uncertain[rn];
            retargetLocations(s, rn, ro);
            // Written straight into the caller's object or a global: handed over, not owned
            // here. Into a local slot: an obligation of this function.
            const LocationKind kind = facts.locations[dst].kind;
            const bool handedOver =
                kind == LocationKind::NonLocal || kind == LocationKind::ArgPointee;
            s.resources[rn] = StateSet::of(handedOver ? OwnState::Escaped : OwnState::Owned);
            // A contract whose condition was not tested: acquired or not, both possible.
            if (certainty == Certainty::Conditional)
                s.resources[rn] |= StateSet::of(OwnState::NotOwned);
            s.uncertain[rn] = certainty == Certainty::Unknown;

            Contents& c = s.locations[dst];
            if (strong)
                c = Contents{};
            c.add(rn);
        }

    } // namespace

    StateSet applyTransformer(const ParamTransformer& t, StateSet in)
    {
        StateSet out = StateSet::none();
        for (std::size_t i = 0; i < t.images.size(); ++i)
        {
            if (in.bits & (1u << i))
                out |= t[i];
        }
        return out;
    }

    ParamTransformer composeTransformers(const ParamTransformer& first,
                                         const ParamTransformer& then)
    {
        ParamTransformer out;
        out.uncertainInputs = first.uncertainInputs;
        for (std::size_t i = 0; i < out.images.size(); ++i)
        {
            out[i] = applyTransformer(then, first[i]);
            if (then.isUncertain(first[i]))
                out.uncertainInputs |= StateSet::of(static_cast<OwnState>(i));
        }
        return out;
    }

    void joinTransformer(ParamTransformer& into, const ParamTransformer& other)
    {
        for (std::size_t i = 0; i < into.images.size(); ++i)
            into[i] |= other[i];
        into.uncertainInputs |= other.uncertainInputs;
    }

    namespace
    {
        /// An Exit may declare that some resources cannot be judged there (an exceptional
        /// exit taken before the very call that would release them). The marking applies to
        /// the recorded state only, never to the path that continues.
        void markExitUncertainty(const Event& exitEvent, AbstractState& recorded)
        {
            for (const LocationId loc : exitEvent.args)
            {
                if (loc >= recorded.locations.size())
                    continue;
                for (const ResourceId r : recorded.locations[loc].resources)
                    recorded.uncertain[r] = true;
            }
        }
    } // namespace

    void applyEvent(const OwnershipFacts& facts, const Event& e, AbstractState& s)
    {
        if (!s.reached)
            return;

        switch (e.kind)
        {
        case Event::Kind::Acquire:
            acquireInto(facts, s, e.site, e.dst, e.strong, e.certainty);
            break;

        case Event::Kind::Release:
        {
            const Contents held = s.locations[e.src];
            markHeld(s, held, StateSet::of(OwnState::Released),
                     e.certainty == Certainty::Guaranteed);
            break;
        }

        case Event::Kind::Copy:
        {
            const Contents src = s.locations[e.src];
            Contents& dst = s.locations[e.dst];
            if (e.strong && facts.locations[e.dst].strongUpdatable)
                dst = src;
            else
                dst.merge(src);
            const LocationKind kind = facts.locations[e.dst].kind;
            if (kind == LocationKind::NonLocal || kind == LocationKind::ArgPointee)
                markHeld(s, src, StateSet::of(OwnState::Escaped), /*guaranteed=*/true);
            break;
        }

        case Event::Kind::Overwrite:
        {
            Contents& dst = s.locations[e.dst];
            Contents value;
            if (e.unknownValue)
                value.mayUnknown = true;
            else
                value.mayNull = true;
            if (e.strong && facts.locations[e.dst].strongUpdatable)
                dst = value;
            else
                dst.merge(value);
            break;
        }

        case Event::Kind::Return:
        {
            const Contents held = s.locations[e.src];
            markHeld(s, held, StateSet::of(OwnState::Escaped), /*guaranteed=*/true);
            break;
        }

        case Event::Kind::AddressEscape:
        {
            Contents& c = s.locations[e.dst];
            for (const ResourceId r : c.resources)
            {
                s.resources[r] |= StateSet::of(OwnState::Escaped);
                s.uncertain[r] = true;
            }
            c.mayUnknown = true;
            break;
        }

        case Event::Kind::UnknownCall:
        {
            for (const LocationId loc : e.args)
            {
                for (const ResourceId r : s.locations[loc].resources)
                {
                    s.resources[r] |=
                        StateSet::of(OwnState::Released) | StateSet::of(OwnState::Escaped);
                    s.uncertain[r] = true;
                }
            }
            break;
        }

        case Event::Kind::Call:
        {
            for (const auto& [loc, transformer] : e.call.params)
            {
                const Contents held = s.locations[loc];
                for (const ResourceId r : held.resources)
                {
                    s.uncertain[r] = s.uncertain[r] || transformer.isUncertain(s.resources[r]);
                    const StateSet image = applyTransformer(transformer, s.resources[r]);
                    if (held.isExactly(r))
                        s.resources[r] = image;
                    else
                        s.resources[r] |= image;
                }
            }
            if (e.call.retDest)
            {
                acquireInto(facts, s, e.call.site, *e.call.retDest, /*strong=*/true,
                            e.call.retCertainty);
            }
            for (std::size_t i = 0; i < e.call.outArgs.size(); ++i)
            {
                const auto& [loc, certainty] = e.call.outArgs[i];
                const std::uint32_t site =
                    i < e.call.outArgSites.size() ? e.call.outArgSites[i] : e.call.site;
                // The callee certainly wrote the slot: a strong update, whatever else may
                // touch the slot elsewhere.
                acquireInto(facts, s, site, loc, /*strong=*/certainty == Certainty::Guaranteed,
                            certainty);
            }
            break;
        }

        case Event::Kind::ContractResolved:
        {
            const ResourceId rn = newInstanceOf(e.site);
            if (!e.unknownValue)
            {
                // Acquired: the "not acquired" alternative is gone.
                s.resources[rn].bits &=
                    static_cast<std::uint8_t>(~StateSet::of(OwnState::NotOwned).bits);
                if (s.resources[rn].empty())
                    s.resources[rn] = StateSet::of(OwnState::Owned);
                break;
            }
            s.resources[rn] = StateSet::of(OwnState::NotOwned);
            for (Contents& c : s.locations)
            {
                if (!c.holds(rn))
                    continue;
                c.remove(rn);
                c.mayNull = true;
            }
            break;
        }

        case Event::Kind::Exit:
            break;
        }
    }

    OwnershipResult solve(const OwnershipFacts& facts, const AbstractState& entry,
                          unsigned iterationLimit)
    {
        OwnershipResult result;
        const std::size_t blockCount = facts.blocks.size();
        const AbstractState bottom =
            AbstractState::bottom(entry.resources.size(), entry.locations.size());
        result.in.assign(blockCount, bottom);
        result.out.assign(blockCount, bottom);
        if (blockCount == 0)
            return result;

        std::vector<std::vector<const Edge*>> successors(blockCount);
        for (const Edge& e : facts.edges)
        {
            if (e.from < blockCount && e.to < blockCount)
                successors[e.from].push_back(&e);
        }

        result.in[0] = entry;
        const unsigned maxIterations = iterationLimit != 0
                                           ? iterationLimit
                                           : std::max(64u, static_cast<unsigned>(blockCount) * 16u);

        std::deque<std::uint32_t> worklist{0};
        std::vector<bool> queued(blockCount, false);
        queued[0] = true;
        unsigned iterations = 0;

        while (!worklist.empty())
        {
            if (++iterations > maxIterations)
            {
                result.incomplete = true;
                return result;
            }
            const std::uint32_t block = worklist.front();
            worklist.pop_front();
            queued[block] = false;

            AbstractState state = result.in[block];
            for (const Event& e : facts.blocks[block].events)
                applyEvent(facts, e, state);
            result.out[block] = state;

            for (const Edge* edge : successors[block])
            {
                AbstractState along = state;
                for (const Event& e : edge->events)
                    applyEvent(facts, e, along);
                AbstractState merged = result.in[edge->to];
                merged.join(along);
                if (!(merged == result.in[edge->to]))
                {
                    result.in[edge->to] = std::move(merged);
                    if (!queued[edge->to])
                    {
                        queued[edge->to] = true;
                        worklist.push_back(edge->to);
                    }
                }
            }
        }

        // Exits are read off the stabilised states, in block/event order; an Exit on an
        // edge sees the source block's out state after the edge's own events.
        for (std::uint32_t block = 0; block < blockCount; ++block)
        {
            if (!result.in[block].reached)
                continue;
            AbstractState state = result.in[block];
            for (std::uint32_t i = 0; i < facts.blocks[block].events.size(); ++i)
            {
                const Event& e = facts.blocks[block].events[i];
                if (e.kind == Event::Kind::Exit)
                {
                    ExitRecord record;
                    record.state = state;
                    markExitUncertainty(e, record.state);
                    record.block = block;
                    record.eventIndex = i;
                    record.exceptional = e.exceptional;
                    result.exits.push_back(std::move(record));
                }
                applyEvent(facts, e, state);
            }
            for (std::uint32_t edgeIndex = 0; edgeIndex < facts.edges.size(); ++edgeIndex)
            {
                const Edge& edge = facts.edges[edgeIndex];
                if (edge.from != block)
                    continue;
                AbstractState along = state;
                for (std::uint32_t i = 0; i < edge.events.size(); ++i)
                {
                    const Event& e = edge.events[i];
                    if (e.kind == Event::Kind::Exit)
                    {
                        ExitRecord record;
                        record.state = along;
                        markExitUncertainty(e, record.state);
                        record.block = block;
                        record.eventIndex = i;
                        record.edge = edgeIndex;
                        record.exceptional = e.exceptional;
                        result.exits.push_back(std::move(record));
                    }
                    applyEvent(facts, e, along);
                }
            }
        }
        return result;
    }

    void
    replay(const OwnershipFacts& facts, const OwnershipResult& result, std::uint32_t block,
           const std::function<void(std::uint32_t, const AbstractState&, const AbstractState&)>& cb)
    {
        if (block >= result.in.size() || !result.in[block].reached)
            return;
        AbstractState state = result.in[block];
        for (std::uint32_t i = 0; i < facts.blocks[block].events.size(); ++i)
        {
            const AbstractState before = state;
            applyEvent(facts, facts.blocks[block].events[i], state);
            cb(i, before, state);
        }
    }

    namespace
    {
        constexpr std::array<OwnState, 4> kAllStates = {OwnState::NotOwned, OwnState::Owned,
                                                        OwnState::Released, OwnState::Escaped};

        /// Whether the fresh resources of the function end up exactly/possibly in a location
        /// of the given kind on the exits of the given kind.
        Certainty freshResourceCertainty(const OwnershipFacts& facts, const OwnershipResult& res,
                                         bool exceptional, LocationKind kind, const ArgPath& path,
                                         ResourceId paramResourceLowerBound)
        {
            bool sawExit = false;
            bool always = true;
            bool sometimes = false;
            for (const ExitRecord& exit : res.exits)
            {
                if (exit.exceptional != exceptional)
                    continue;
                sawExit = true;
                bool exact = false;
                bool possible = false;
                for (LocationId loc = 0; loc < facts.locations.size(); ++loc)
                {
                    const Location& location = facts.locations[loc];
                    if (location.kind != kind ||
                        (kind == LocationKind::ArgPointee && !(location.path == path)))
                        continue;
                    const Contents& c = exit.state.locations[loc];
                    for (const ResourceId r : c.resources)
                    {
                        if (r >= paramResourceLowerBound)
                            continue; // a parameter's resource, not a fresh one
                        if (exit.state.uncertain[r])
                            return Certainty::Unknown;
                        if (exit.state.resources[r].isOnly(OwnState::NotOwned))
                            continue;
                        possible = true;
                        // Returned/stored through an out-arg it is Escaped; acquired straight
                        // into the out-arg it is still Owned. Both are fresh handovers.
                        if (c.isExactly(r) && !exit.state.resources[r].has(OwnState::NotOwned))
                            exact = true;
                    }
                }
                always = always && exact;
                sometimes = sometimes || possible;
            }
            if (!sawExit || !sometimes)
                return Certainty::Unknown;
            return always ? Certainty::Guaranteed : Certainty::Conditional;
        }
    } // namespace

    namespace
    {
        /// The kind of the fresh resource found in the given locations on the given exits.
        std::string freshResourceKind(const OwnershipFacts& facts, const OwnershipResult& res,
                                      bool exceptional, LocationKind kind, const ArgPath& path,
                                      ResourceId paramResourceLowerBound)
        {
            for (const ExitRecord& exit : res.exits)
            {
                if (exit.exceptional != exceptional)
                    continue;
                for (LocationId loc = 0; loc < facts.locations.size(); ++loc)
                {
                    const Location& location = facts.locations[loc];
                    if (location.kind != kind ||
                        (kind == LocationKind::ArgPointee && !(location.path == path)))
                        continue;
                    for (const ResourceId r : exit.state.locations[loc].resources)
                    {
                        if (r >= paramResourceLowerBound)
                            continue;
                        const std::uint32_t site = r / 2u;
                        if (site < facts.siteKinds.size())
                            return facts.siteKinds[site];
                    }
                }
            }
            return {};
        }
    } // namespace

    FunctionOwnershipSummary computeSummary(const OwnershipFacts& facts, unsigned iterationLimit)
    {
        FunctionOwnershipSummary summary;

        // Parameters (by value, then pointees) get their own resources, numbered after the
        // function's sites.
        const std::uint32_t paramSiteBase = facts.siteCount;
        const std::size_t paramTotal = facts.paramLocations.size() + facts.pointeeLocations.size();
        const std::size_t resourceCount = 2u * (paramSiteBase + paramTotal);
        const auto paramLocationAt = [&](std::size_t p) -> LocationId
        {
            return p < facts.paramLocations.size()
                       ? facts.paramLocations[p].second
                       : facts.pointeeLocations[p - facts.paramLocations.size()].second;
        };

        const auto entryWith = [&](std::size_t paramIndex, OwnState state)
        {
            AbstractState entry = AbstractState::entry(resourceCount, facts.locations.size());
            for (std::size_t p = 0; p < paramTotal; ++p)
            {
                const ResourceId r = newInstanceOf(paramSiteBase + static_cast<std::uint32_t>(p));
                entry.locations[paramLocationAt(p)].add(r);
                entry.resources[r] = StateSet::of(p == paramIndex ? state : OwnState::Owned);
            }
            return entry;
        };

        bool anyNormal = false;
        bool anyExceptional = false;
        for (std::size_t p = 0; p < paramTotal; ++p)
        {
            const ResourceId r = newInstanceOf(paramSiteBase + static_cast<std::uint32_t>(p));
            ParamTransformer normal{};
            ParamTransformer exceptional{};
            for (const OwnState state : kAllStates)
            {
                const OwnershipResult res = solve(facts, entryWith(p, state), iterationLimit);
                if (res.incomplete)
                {
                    summary.incomplete = true;
                    return summary;
                }
                for (const ExitRecord& exit : res.exits)
                {
                    ParamTransformer& t = exit.exceptional ? exceptional : normal;
                    t[static_cast<std::size_t>(state)] |= exit.state.resources[r];
                    if (exit.state.uncertain[r])
                        t.uncertainInputs |= StateSet::of(state);
                    (exit.exceptional ? anyExceptional : anyNormal) = true;
                }
            }
            if (p < facts.paramLocations.size())
            {
                const unsigned argIndex = facts.paramLocations[p].first;
                summary.normal.params[argIndex] = normal;
                summary.exceptional.params[argIndex] = exceptional;
            }
            else
            {
                const ArgPath& path = facts.pointeeLocations[p - facts.paramLocations.size()].first;
                summary.normal.pointeeParams[path] = normal;
                summary.exceptional.pointeeParams[path] = exceptional;
            }
        }

        // Fresh resources: one solve with every parameter Owned (or none).
        const OwnershipResult res =
            solve(facts, entryWith(paramTotal, OwnState::Owned), iterationLimit);
        if (res.incomplete)
        {
            summary.incomplete = true;
            return summary;
        }
        for (const ExitRecord& exit : res.exits)
            (exit.exceptional ? anyExceptional : anyNormal) = true;
        const ResourceId paramResourceLowerBound = newInstanceOf(paramSiteBase);
        summary.normal.returns.certainty = freshResourceCertainty(
            facts, res, false, LocationKind::Return, ArgPath{}, paramResourceLowerBound);
        summary.normal.returns.kind = freshResourceKind(facts, res, false, LocationKind::Return,
                                                        ArgPath{}, paramResourceLowerBound);
        summary.exceptional.returns.certainty = Certainty::Unknown;
        for (LocationId loc = 0; loc < facts.locations.size(); ++loc)
        {
            const Location& location = facts.locations[loc];
            if (location.kind != LocationKind::ArgPointee)
                continue;
            for (const bool exceptional : {false, true})
            {
                const Certainty c =
                    freshResourceCertainty(facts, res, exceptional, LocationKind::ArgPointee,
                                           location.path, paramResourceLowerBound);
                if (c == Certainty::Unknown)
                    continue;
                FreshResource fresh;
                fresh.certainty = c;
                fresh.kind = freshResourceKind(facts, res, exceptional, LocationKind::ArgPointee,
                                               location.path, paramResourceLowerBound);
                (exceptional ? summary.exceptional : summary.normal).outArgs[location.path] =
                    std::move(fresh);
            }
        }
        summary.normal.present = anyNormal;
        summary.exceptional.present = anyExceptional;
        return summary;
    }
} // namespace ctrace::stack::analysis::ownership
