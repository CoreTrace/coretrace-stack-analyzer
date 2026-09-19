// SPDX-License-Identifier: Apache-2.0
#include "analysis/ownership/OwnershipEngine.hpp"

#include <algorithm>
#include <deque>

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
                if (held.isExactly(r) && guaranteed)
                    s.resources[r] = add;
                else
                    s.resources[r] |= add;
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

        void acquireInto(AbstractState& s, std::uint32_t site, LocationId dst, bool strong,
                         Certainty certainty)
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
            s.resources[rn] = StateSet::of(OwnState::Owned);
            s.uncertain[rn] = certainty == Certainty::Unknown;

            Contents& c = s.locations[dst];
            if (strong)
                c = Contents{};
            c.add(rn);
        }

        StateSet applyTransformer(const ParamTransformer& t, StateSet in)
        {
            StateSet out = StateSet::none();
            for (std::size_t i = 0; i < t.size(); ++i)
            {
                if (in.bits & (1u << i))
                    out |= t[i];
            }
            return out;
        }
    } // namespace

    void applyEvent(const OwnershipFacts& facts, const Event& e, AbstractState& s)
    {
        if (!s.reached)
            return;

        switch (e.kind)
        {
        case Event::Kind::Acquire:
            acquireInto(s, e.site, e.dst, e.strong, e.certainty);
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
                    const StateSet image = applyTransformer(transformer, s.resources[r]);
                    if (held.isExactly(r))
                        s.resources[r] = image;
                    else
                        s.resources[r] |= image;
                }
            }
            if (e.call.retDest)
            {
                acquireInto(s, e.call.site, *e.call.retDest, /*strong=*/true, e.call.retCertainty);
            }
            for (const auto& [loc, certainty] : e.call.outArgs)
            {
                acquireInto(s, e.call.site, loc,
                            /*strong=*/facts.locations[loc].strongUpdatable, certainty);
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

        // Exits are read off the stabilised states, in block/event order.
        for (std::uint32_t block = 0; block < blockCount; ++block)
        {
            if (!result.in[block].reached)
                continue;
            AbstractState state = result.in[block];
            for (std::uint32_t i = 0; i < facts.blocks[block].events.size(); ++i)
            {
                const Event& e = facts.blocks[block].events[i];
                if (e.kind == Event::Kind::Exit)
                    result.exits.push_back({block, i, e.exceptional, state});
                applyEvent(facts, e, state);
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
} // namespace ctrace::stack::analysis::ownership
