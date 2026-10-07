// SPDX-License-Identifier: Apache-2.0
// Unit tests for the LLVM-free ownership engine (docs/superpowers/specs/
// 2026-09-19-resource-ownership-engine-design.md). Facts are built by hand.
#include "analysis/ownership/OwnershipDomain.hpp"
#include "analysis/ownership/OwnershipEngine.hpp"
#include "analysis/ownership/OwnershipFacts.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <string>

namespace
{
    struct TestReport
    {
        int failures = 0;

        void expect(bool condition, const std::string& message)
        {
            if (!condition)
            {
                ++failures;
                std::cerr << "[FAIL] " << message << "\n";
            }
            else
            {
                std::cout << "[PASS] " << message << "\n";
            }
        }
    };

    bool testDomain(TestReport& r)
    {
        using namespace ctrace::stack::analysis::ownership;

        StateSet s = StateSet::of(OwnState::Owned);
        r.expect(s.isOnly(OwnState::Owned), "Domain: singleton isOnly");
        s |= StateSet::of(OwnState::Released);
        r.expect(s.has(OwnState::Owned) && s.has(OwnState::Released) && !s.isOnly(OwnState::Owned),
                 "Domain: union");
        r.expect(StateSet::none().empty(), "Domain: none is empty");

        AbstractState b = AbstractState::bottom(2, 1);
        AbstractState e = AbstractState::entry(2, 1);
        r.expect(!b.reached && e.reached && e.resources[0].isOnly(OwnState::NotOwned) &&
                     e.locations[0].empty(),
                 "Domain: bottom vs entry");
        AbstractState j = b;
        j.join(e);
        r.expect(j == e, "Domain: bottom is the join identity");
        AbstractState k = e;
        k.join(b);
        r.expect(k == e, "Domain: joining bottom changes nothing");

        Contents c;
        c.add(3);
        r.expect(c.isExactly(3), "Domain: exact contents");
        c.add(3);
        r.expect(c.resources.size() == 1, "Domain: add is idempotent");
        c.mayNull = true;
        r.expect(!c.isExactly(3), "Domain: null spoils exactness");
        Contents d;
        d.add(1);
        d.merge(c);
        r.expect(d.resources.size() == 2 && d.resources[0] == 1 && d.resources[1] == 3 && d.mayNull,
                 "Domain: merge keeps contents sorted and unique");
        return r.failures == 0;
    }
    // ---- helpers to build synthetic facts -------------------------------------------------
    using namespace ctrace::stack::analysis::ownership;

    Event acquire(std::uint32_t site, LocationId dst, bool strong = true,
                  Certainty c = Certainty::Guaranteed)
    {
        Event e;
        e.kind = Event::Kind::Acquire;
        e.site = site;
        e.dst = dst;
        e.strong = strong;
        e.certainty = c;
        return e;
    }
    Event release(LocationId src, Certainty c = Certainty::Guaranteed)
    {
        Event e;
        e.kind = Event::Kind::Release;
        e.src = src;
        e.certainty = c;
        return e;
    }
    Event copy(LocationId dst, LocationId src, bool strong = true)
    {
        Event e;
        e.kind = Event::Kind::Copy;
        e.dst = dst;
        e.src = src;
        e.strong = strong;
        return e;
    }
    Event overwrite(LocationId dst, bool unknownValue = false, bool strong = true)
    {
        Event e;
        e.kind = Event::Kind::Overwrite;
        e.dst = dst;
        e.unknownValue = unknownValue;
        e.strong = strong;
        return e;
    }
    Event ret(LocationId src)
    {
        Event e;
        e.kind = Event::Kind::Return;
        e.src = src;
        return e;
    }
    Event exit(bool exceptional = false)
    {
        Event e;
        e.kind = Event::Kind::Exit;
        e.exceptional = exceptional;
        return e;
    }
    Event addressEscape(LocationId dst)
    {
        Event e;
        e.kind = Event::Kind::AddressEscape;
        e.dst = dst;
        return e;
    }
    Event callWith(LocationId loc, StateSet imageOfOwned)
    {
        Event e;
        e.kind = Event::Kind::Call;
        ParamTransformer t = identityTransformer();
        t[static_cast<std::size_t>(OwnState::Owned)] = imageOfOwned;
        e.call.params.push_back({loc, t});
        return e;
    }

    OwnershipFacts facts(std::size_t blocks, std::vector<Edge> edges, std::size_t locations,
                         std::uint32_t sites)
    {
        OwnershipFacts f;
        f.blocks.resize(blocks);
        f.edges = std::move(edges);
        f.locations.resize(locations);
        f.siteCount = sites;
        return f;
    }
    Edge edge(std::uint32_t from, std::uint32_t to, std::vector<Event> events = {})
    {
        Edge e;
        e.from = from;
        e.to = to;
        e.events = std::move(events);
        return e;
    }
    AbstractState entryOf(const OwnershipFacts& f)
    {
        return AbstractState::entry(2 * f.siteCount, f.locations.size());
    }

    bool testEngine(TestReport& r)
    {
        const ResourceId n0 = newInstanceOf(0);
        const ResourceId o0 = oldInstancesOf(0);

        {
            OwnershipFacts f = facts(1, {}, 1, 1);
            f.blocks[0].events = {acquire(0, 0), exit()};
            const OwnershipResult res = solve(f, entryOf(f));
            r.expect(!res.incomplete && res.exits.size() == 1 &&
                         res.exits[0].state.resources[n0].isOnly(OwnState::Owned),
                     "Engine: acquire then exit leaves Owned");
        }
        {
            OwnershipFacts f = facts(1, {}, 1, 1);
            f.blocks[0].events = {acquire(0, 0), release(0), exit()};
            const OwnershipResult res = solve(f, entryOf(f));
            r.expect(res.exits.size() == 1 &&
                         res.exits[0].state.resources[n0].isOnly(OwnState::Released),
                     "Engine: release is strong on exact contents");
        }
        {
            // 0: acquire; 0→1 releases, 0→2 does not; both → 3: exit.
            OwnershipFacts f =
                facts(4, {edge(0, 1, {release(0)}), edge(0, 2), edge(1, 3), edge(2, 3)}, 1, 1);
            f.blocks[0].events = {acquire(0, 0)};
            f.blocks[3].events = {exit()};
            const OwnershipResult res = solve(f, entryOf(f));
            const StateSet st = res.exits.at(0).state.resources[n0];
            r.expect(st.has(OwnState::Owned) && st.has(OwnState::Released) &&
                         !st.has(OwnState::NotOwned),
                     "Engine: conditional release joins to {Owned, Released}");
        }
        {
            // Block 1 has no incoming edge: its acquire must not exist.
            OwnershipFacts f = facts(3, {edge(0, 2)}, 1, 1);
            f.blocks[1].events = {acquire(0, 0)};
            f.blocks[2].events = {exit()};
            const OwnershipResult res = solve(f, entryOf(f));
            r.expect(!res.out[1].reached &&
                         res.exits.at(0).state.resources[n0].isOnly(OwnState::NotOwned),
                     "Engine: unreachable block stays bottom");
        }
        {
            OwnershipFacts f = facts(1, {}, 2, 1);
            f.blocks[0].events = {acquire(0, 0), copy(1, 0), ret(1), exit()};
            const OwnershipResult res = solve(f, entryOf(f));
            r.expect(res.exits.at(0).state.resources[n0].isOnly(OwnState::Escaped),
                     "Engine: return of exact contents escapes");
        }
        {
            // ret slot 1 = h, then maybe overwritten with null, then returned.
            OwnershipFacts f =
                facts(4, {edge(0, 1, {overwrite(1)}), edge(0, 2), edge(1, 3), edge(2, 3)}, 2, 1);
            f.blocks[0].events = {acquire(0, 0), copy(1, 0)};
            f.blocks[3].events = {ret(1), exit()};
            const OwnershipResult res = solve(f, entryOf(f));
            const StateSet st = res.exits.at(0).state.resources[n0];
            r.expect(st.has(OwnState::Owned) && st.has(OwnState::Escaped),
                     "Engine: return of overwritten slot keeps Owned");
        }
        {
            // saved = h; acquire(&h) again; release(saved); release(h).
            OwnershipFacts f = facts(1, {}, 2, 1);
            f.blocks[0].events = {acquire(0, 0), copy(1, 0), acquire(0, 0),
                                  release(1),    release(0), exit()};
            const OwnershipResult res = solve(f, entryOf(f));
            bool sawOldInSaved = false;
            replay(f, res, 0,
                   [&](std::uint32_t idx, const AbstractState&, const AbstractState& after)
                   {
                       if (idx == 2)
                           sawOldInSaved = after.locations[1].isExactly(o0) &&
                                           after.resources[o0].isOnly(OwnState::Owned) &&
                                           after.resources[n0].isOnly(OwnState::Owned);
                   });
            r.expect(sawOldInSaved, "Engine: alias keeps the old resource reachable");
            r.expect(res.exits.at(0).state.resources[o0].isOnly(OwnState::Released) &&
                         res.exits.at(0).state.resources[n0].isOnly(OwnState::Released),
                     "Engine: both instances end released through their references");
        }
        {
            // 0 → 1 (acquire) → 1 (back-edge) and → 2 (exit).
            OwnershipFacts f = facts(3, {edge(0, 1), edge(1, 1), edge(1, 2)}, 1, 1);
            f.blocks[1].events = {acquire(0, 0)};
            f.blocks[2].events = {exit()};
            const OwnershipResult res = solve(f, entryOf(f));
            const AbstractState& s = res.exits.at(0).state;
            r.expect(s.resources[o0].has(OwnState::Owned) &&
                         s.resources[n0].isOnly(OwnState::Owned),
                     "Engine: loop reacquire accumulates into old");
        }
        {
            OwnershipFacts f = facts(1, {}, 1, 1);
            f.blocks[0].events = {acquire(0, 0), addressEscape(0), exit()};
            const OwnershipResult res = solve(f, entryOf(f));
            r.expect(res.exits.at(0).state.uncertain[n0], "Engine: address escape marks uncertain");
        }
        {
            OwnershipFacts f = facts(1, {}, 1, 1);
            f.blocks[0].events = {acquire(0, 0), callWith(0, StateSet::of(OwnState::Released)),
                                  exit()};
            const OwnershipResult res = solve(f, entryOf(f));
            r.expect(res.exits.at(0).state.resources[n0].isOnly(OwnState::Released),
                     "Engine: call transformer release-always");
        }
        {
            OwnershipFacts f = facts(1, {}, 1, 1);
            f.blocks[0].events = {
                acquire(0, 0),
                callWith(0, StateSet::of(OwnState::Owned) | StateSet::of(OwnState::Released)),
                exit()};
            const OwnershipResult res = solve(f, entryOf(f));
            const StateSet st = res.exits.at(0).state.resources[n0];
            r.expect(st.has(OwnState::Owned) && st.has(OwnState::Released),
                     "Engine: call transformer release-sometimes");
        }
        {
            OwnershipFacts f = facts(1, {}, 1, 1);
            f.blocks[0].events = {acquire(0, 0), exit(true), release(0), exit()};
            const OwnershipResult res = solve(f, entryOf(f));
            r.expect(res.exits.size() == 2 && res.exits[0].exceptional &&
                         res.exits[0].state.resources[n0].isOnly(OwnState::Owned) &&
                         !res.exits[1].exceptional &&
                         res.exits[1].state.resources[n0].isOnly(OwnState::Released),
                     "Engine: exceptional exit sees the state before the call effects");
        }
        {
            OwnershipFacts f = facts(3, {edge(0, 1), edge(1, 1), edge(1, 2)}, 1, 1);
            f.blocks[1].events = {acquire(0, 0)};
            f.blocks[2].events = {exit()};
            const OwnershipResult res = solve(f, entryOf(f), /*iterationLimit=*/1);
            r.expect(res.incomplete && res.exits.empty(), "Engine: budget exhaustion is explicit");
        }
        return r.failures == 0;
    }
    // Summary-mode facts: parameter 0 is passed in location 0 holding resource `param`.
    OwnershipFacts wrapperFacts(std::size_t blocks, std::vector<Edge> edges, std::size_t locations,
                                std::uint32_t sites)
    {
        OwnershipFacts f = facts(blocks, std::move(edges), locations, sites);
        f.paramLocations = {{0, 0}};
        return f;
    }
    Event callEvent(CallEffect effect)
    {
        Event e;
        e.kind = Event::Kind::Call;
        e.call = std::move(effect);
        return e;
    }

    Event contractResolved(std::uint32_t site, bool acquired)
    {
        Event e;
        e.kind = Event::Kind::ContractResolved;
        e.site = site;
        e.unknownValue = !acquired;
        return e;
    }

    bool testEdgeExits(TestReport& r)
    {
        const ResourceId n0 = newInstanceOf(0);
        // 0: acquire h; 0→1: retval = null (after a release); 0→2: retval = h;
        // both → 3: the merged `ret retval`, whose Copy/Return/Exit are placed on the
        // incoming edges so that each path is judged before the join.
        OwnershipFacts f = facts(4,
                                 {edge(0, 1), edge(0, 2), edge(1, 3, {copy(2, 1), ret(2), exit()}),
                                  edge(2, 3, {copy(2, 1), ret(2), exit()})},
                                 3, 1);
        f.locations[2].kind = LocationKind::Return;
        f.blocks[0].events = {acquire(0, 0)};
        f.blocks[1].events = {release(0), overwrite(1)};
        f.blocks[2].events = {copy(1, 0)};
        const OwnershipResult res = solve(f, entryOf(f));
        r.expect(res.exits.size() == 2, "EdgeExits: one exit per incoming edge");
        bool sawReleased = false;
        bool sawEscaped = false;
        for (const ExitRecord& e : res.exits)
        {
            sawReleased = sawReleased || e.state.resources[n0].isOnly(OwnState::Released);
            sawEscaped = sawEscaped || e.state.resources[n0].isOnly(OwnState::Escaped);
        }
        r.expect(sawReleased && sawEscaped,
                 "EdgeExits: each path keeps its own verdict (released / escaped), no leak");
        return r.failures == 0;
    }

    bool testContracts(TestReport& r)
    {
        const ResourceId n0 = newInstanceOf(0);
        {
            // p = acquire() [if_ret!=null]; if (!p) return; release(p); return.
            OwnershipFacts f = facts(
                3,
                {edge(0, 1, {contractResolved(0, false)}), edge(0, 2, {contractResolved(0, true)})},
                1, 1);
            f.blocks[0].events = {acquire(0, 0, true, Certainty::Conditional)};
            f.blocks[1].events = {exit()};
            f.blocks[2].events = {release(0), exit()};
            const OwnershipResult res = solve(f, entryOf(f));
            r.expect(res.exits.size() == 2 &&
                         res.exits[0].state.resources[n0].isOnly(OwnState::NotOwned) &&
                         res.exits[0].state.locations[0].mayNull &&
                         !res.exits[0].state.locations[0].holds(n0),
                     "Contracts: the failure edge holds nothing and the slot is null");
            r.expect(res.exits[1].state.resources[n0].isOnly(OwnState::Released),
                     "Contracts: the success edge owns the resource and releases it");
        }
        {
            OwnershipFacts f = facts(1, {}, 1, 1);
            f.blocks[0].events = {acquire(0, 0, true, Certainty::Conditional), exit()};
            const OwnershipResult res = solve(f, entryOf(f));
            const StateSet st = res.exits.at(0).state.resources[n0];
            r.expect(st.has(OwnState::Owned) && st.has(OwnState::NotOwned),
                     "Contracts: an untested contract leaves both outcomes possible");
        }
        return r.failures == 0;
    }

    bool testExitUncertainty(TestReport& r)
    {
        const ResourceId n0 = newInstanceOf(0);
        // An exceptional exit taken before the call that releases the resource: whether the
        // release happened is unknowable, so the resource is uncertain *there* only.
        OwnershipFacts f = facts(1, {}, 1, 1);
        Event exceptional = exit(true);
        exceptional.args = {0};
        f.blocks[0].events = {acquire(0, 0), exceptional, release(0), exit()};
        const OwnershipResult res = solve(f, entryOf(f));
        r.expect(res.exits.size() == 2 && res.exits[0].exceptional &&
                     res.exits[0].state.uncertain[n0],
                 "ExitUncertainty: the exceptional exit of a releasing call is uncertain");
        r.expect(!res.exits[1].state.uncertain[n0] &&
                     res.exits[1].state.resources[n0].isOnly(OwnState::Released),
                 "ExitUncertainty: the normal path is unaffected");
        return r.failures == 0;
    }

    bool testSummaryMetadata(TestReport& r)
    {
        const StateSet owned = StateSet::of(OwnState::Owned);
        OwnershipFacts callee = facts(1, {}, 1, 0);
        callee.paramLocations.push_back({0, 0});
        Event unknown;
        unknown.kind = Event::Kind::UnknownCall;
        unknown.args = {0};
        callee.blocks[0].events = {unknown, exit()};
        const FunctionOwnershipSummary summary = computeSummary(callee);

        Event call;
        call.kind = Event::Kind::Call;
        call.call.params.push_back({0, summary.normal.params.at(0)});
        OwnershipFacts caller = facts(1, {}, 1, 1);
        caller.blocks[0].events = {acquire(0, 0), call, exit()};
        const OwnershipResult result = solve(caller, entryOf(caller));
        r.expect(result.exits.at(0).state.uncertain[newInstanceOf(0)],
                 "Summary: opaque wrapper preserves caller uncertainty");
        callee.blocks[0].events = {call, exit()};
        r.expect(computeSummary(callee).normal.params.at(0).isUncertain(owned),
                 "Summary: uncertainty survives nested wrappers");

        callee.paramLocations.clear();
        callee.locations[0].kind = LocationKind::ArgPointee;
        callee.pointeeLocations.push_back({ArgPath{}, 0});
        callee.blocks[0].events = {addressEscape(0), exit(true)};
        r.expect(computeSummary(callee).exceptional.pointeeParams.at(ArgPath{}).isUncertain(owned),
                 "Summary: exceptional pointee uncertainty survives export");

        ParamTransformer first = identityTransformer();
        first[static_cast<std::size_t>(OwnState::Owned)] = StateSet::of(OwnState::Released);
        ParamTransformer second = identityTransformer();
        second.uncertainInputs = StateSet::of(OwnState::Released);
        const ParamTransformer composed = composeTransformers(first, second);
        r.expect(composed.isUncertain(owned) &&
                     !composed.isUncertain(StateSet::of(OwnState::NotOwned)),
                 "Summary: composition carries uncertainty for the affected inputs only");
        first.uncertainInputs = owned;
        r.expect(composeTransformers(first, identityTransformer()).isUncertain(owned),
                 "Summary: composition retains earlier uncertainty");
        ParamTransformer joined = identityTransformer();
        joinTransformer(joined, first);
        r.expect(joined.isUncertain(owned), "Summary: joining retains uncertainty");

        OwnershipFacts fresh = facts(1, {}, 1, 1);
        fresh.locations[0].kind = LocationKind::Return;
        fresh.blocks[0].events = {acquire(0, 0, true, Certainty::Conditional), ret(0), exit()};
        r.expect(computeSummary(fresh).normal.returns.certainty == Certainty::Conditional,
                 "Summary: returning a conditional acquisition does not guarantee a resource");
        fresh.locations[0].kind = LocationKind::ArgPointee;
        fresh.blocks[0].events = {acquire(0, 0, true, Certainty::Conditional), exit()};
        r.expect(computeSummary(fresh).normal.outArgs.at(ArgPath{}).certainty ==
                     Certainty::Conditional,
                 "Summary: a conditional out-parameter acquisition stays conditional");
        fresh.locations[0].kind = LocationKind::Return;
        fresh.blocks[0].events = {acquire(0, 0), unknown, ret(0), exit()};
        r.expect(computeSummary(fresh).normal.returns.certainty == Certainty::Unknown,
                 "Summary: an uncertain fresh resource cannot become a guaranteed return");
        return r.failures == 0;
    }

    bool testSummaries(TestReport& r)
    {
        const auto owned = static_cast<std::size_t>(OwnState::Owned);
        {
            OwnershipFacts f = wrapperFacts(1, {}, 1, 1);
            f.blocks[0].events = {release(0), exit()};
            const FunctionOwnershipSummary s = computeSummary(f);
            r.expect(s.normal.present && s.normal.params.at(0)[owned].isOnly(OwnState::Released),
                     "Summary: wrapper releasing always maps Owned to {Released}");
            r.expect(!s.exceptional.present,
                     "Summary: no exceptional exit, no exceptional transformer");
        }
        {
            OwnershipFacts f = wrapperFacts(
                4, {edge(0, 1, {release(0)}), edge(0, 2), edge(1, 3), edge(2, 3)}, 1, 1);
            f.blocks[3].events = {exit()};
            const FunctionOwnershipSummary s = computeSummary(f);
            const StateSet img = s.normal.params.at(0)[owned];
            r.expect(img.has(OwnState::Owned) && img.has(OwnState::Released),
                     "Summary: wrapper releasing sometimes keeps Owned");
        }
        {
            // release(h); *out = acquire(): location 1 is ArgPointee(1).
            OwnershipFacts f = wrapperFacts(1, {}, 2, 1);
            f.locations[1].kind = LocationKind::ArgPointee;
            f.locations[1].path.argIndex = 1;
            f.blocks[0].events = {release(0), acquire(0, 1), exit()};
            const FunctionOwnershipSummary s = computeSummary(f);
            r.expect(s.normal.params.at(0)[owned].isOnly(OwnState::Released) &&
                         s.normal.outArgs.count(ArgPath{0, 1, false}) == 1 &&
                         s.normal.outArgs.at(ArgPath{0, 1, false}).certainty ==
                             Certainty::Guaranteed,
                     "Summary: wrapper releasing then acquiring reports a fresh out-arg");
        }
        {
            // local = acquire(); release(local): nothing remains, parameter untouched.
            OwnershipFacts f = wrapperFacts(1, {}, 2, 1);
            f.blocks[0].events = {acquire(0, 1), release(1), exit()};
            const FunctionOwnershipSummary s = computeSummary(f);
            r.expect(s.normal.params.at(0)[owned].isOnly(OwnState::Owned) &&
                         s.normal.outArgs.empty() &&
                         s.normal.returns.certainty != Certainty::Guaranteed,
                     "Summary: wrapper acquiring then releasing leaves no obligation");
        }
        {
            // Returns a fresh resource on every normal exit: location 1 is the Return location.
            OwnershipFacts f = wrapperFacts(1, {}, 2, 1);
            f.locations[1].kind = LocationKind::Return;
            f.blocks[0].events = {acquire(0, 1), ret(1), exit()};
            const FunctionOwnershipSummary s = computeSummary(f);
            r.expect(s.normal.returns.certainty == Certainty::Guaranteed,
                     "Summary: returning a fresh resource on every exit is Guaranteed");
        }
        {
            // Returns fresh on one path and null on the other → Conditional.
            OwnershipFacts f = wrapperFacts(
                4,
                {edge(0, 1, {acquire(0, 1)}), edge(0, 2, {overwrite(1)}), edge(1, 3), edge(2, 3)},
                2, 1);
            f.locations[1].kind = LocationKind::Return;
            f.blocks[3].events = {ret(1), exit()};
            const FunctionOwnershipSummary s = computeSummary(f);
            r.expect(s.normal.returns.certainty == Certainty::Conditional,
                     "Summary: returning a fresh resource on some exits is Conditional");
        }
        {
            // Exceptional exit before the release, normal exit after it.
            OwnershipFacts f = wrapperFacts(1, {}, 1, 1);
            f.blocks[0].events = {exit(true), release(0), exit()};
            const FunctionOwnershipSummary s = computeSummary(f);
            r.expect(s.exceptional.present &&
                         s.exceptional.params.at(0)[owned].isOnly(OwnState::Owned) &&
                         s.normal.params.at(0)[owned].isOnly(OwnState::Released),
                     "Summary: exceptional and normal exits are summarised separately");
        }
        {
            OwnershipFacts f = wrapperFacts(3, {edge(0, 1), edge(1, 1), edge(1, 2)}, 1, 1);
            f.blocks[1].events = {acquire(0, 0)};
            f.blocks[2].events = {exit()};
            const FunctionOwnershipSummary s = computeSummary(f, /*iterationLimit=*/1);
            r.expect(s.incomplete, "Summary: exhausted budget marks the summary incomplete");
        }
        {
            ParamTransformer always = identityTransformer();
            always[owned] = StateSet::of(OwnState::Released);
            ParamTransformer sometimes = identityTransformer();
            sometimes[owned] = StateSet::of(OwnState::Owned) | StateSet::of(OwnState::Released);
            r.expect(
                applyTransformer(sometimes, StateSet::of(OwnState::Owned)).has(OwnState::Owned),
                "Summary: applyTransformer extends by union");
            const ParamTransformer composed = composeTransformers(sometimes, always);
            r.expect(composed[owned].isOnly(OwnState::Released),
                     "Summary: composing 'sometimes' then 'always' releases");
            ParamTransformer joined = always;
            joinTransformer(joined, identityTransformer());
            r.expect(joined[owned].has(OwnState::Owned) && joined[owned].has(OwnState::Released),
                     "Summary: join of transformers is pointwise union");
        }
        return r.failures == 0;
    }
    // ---- summaries of several parameters at once -------------------------------------------

    // The transformers of the parameters as computeSummary built them before it solved the
    // parameters together: one solve per parameter and entry state, the other parameters Owned,
    // and one solve with every parameter Owned for the fresh resources.
    struct ReferenceTransformers
    {
        std::map<unsigned, ParamTransformer> normalParams;
        std::map<unsigned, ParamTransformer> exceptionalParams;
        std::map<ArgPath, ParamTransformer> normalPointees;
        std::map<ArgPath, ParamTransformer> exceptionalPointees;
        bool anyNormal = false;
        bool anyExceptional = false;
        bool incomplete = false;
    };

    AbstractState summaryEntry(const OwnershipFacts& f, std::size_t paramIndex, OwnState state)
    {
        const std::size_t total = f.paramLocations.size() + f.pointeeLocations.size();
        AbstractState entry = AbstractState::entry(2u * (f.siteCount + total), f.locations.size());
        for (std::size_t p = 0; p < total; ++p)
        {
            const ResourceId r = newInstanceOf(f.siteCount + static_cast<std::uint32_t>(p));
            const LocationId loc = p < f.paramLocations.size()
                                       ? f.paramLocations[p].second
                                       : f.pointeeLocations[p - f.paramLocations.size()].second;
            entry.locations[loc].add(r);
            entry.resources[r] = StateSet::of(p == paramIndex ? state : OwnState::Owned);
        }
        return entry;
    }

    ReferenceTransformers referenceTransformers(const OwnershipFacts& f, unsigned limit)
    {
        ReferenceTransformers ref;
        const std::size_t total = f.paramLocations.size() + f.pointeeLocations.size();
        for (std::size_t p = 0; p < total; ++p)
        {
            const ResourceId r = newInstanceOf(f.siteCount + static_cast<std::uint32_t>(p));
            ParamTransformer normal{};
            ParamTransformer exceptional{};
            for (const OwnState state :
                 {OwnState::NotOwned, OwnState::Owned, OwnState::Released, OwnState::Escaped})
            {
                const OwnershipResult res = solve(f, summaryEntry(f, p, state), limit);
                if (res.incomplete)
                {
                    ref.incomplete = true;
                    return ref;
                }
                for (const ExitRecord& exit : res.exits)
                {
                    ParamTransformer& t = exit.exceptional ? exceptional : normal;
                    t[static_cast<std::size_t>(state)] |= exit.state.resources[r];
                    if (exit.state.uncertain[r])
                        t.uncertainInputs |= StateSet::of(state);
                    (exit.exceptional ? ref.anyExceptional : ref.anyNormal) = true;
                }
            }
            if (p < f.paramLocations.size())
            {
                ref.normalParams[f.paramLocations[p].first] = normal;
                ref.exceptionalParams[f.paramLocations[p].first] = exceptional;
            }
            else
            {
                const ArgPath& path = f.pointeeLocations[p - f.paramLocations.size()].first;
                ref.normalPointees[path] = normal;
                ref.exceptionalPointees[path] = exceptional;
            }
        }
        const OwnershipResult fresh = solve(f, summaryEntry(f, total, OwnState::Owned), limit);
        if (fresh.incomplete)
        {
            ref.incomplete = true;
            return ref;
        }
        for (const ExitRecord& exit : fresh.exits)
            (exit.exceptional ? ref.anyExceptional : ref.anyNormal) = true;
        return ref;
    }

    // computeSummary gives what one solve per parameter gives, and is incomplete exactly when
    // that is.
    void expectSameAsReference(TestReport& r, const OwnershipFacts& f, unsigned limit,
                               const std::string& name)
    {
        const ReferenceTransformers ref = referenceTransformers(f, limit);
        const FunctionOwnershipSummary s = computeSummary(f, limit);
        r.expect(s.incomplete == ref.incomplete,
                 "Joint summary: " + name + ": incomplete exactly when one solve per parameter is");
        if (s.incomplete || ref.incomplete)
            return;
        r.expect(
            s.normal.params == ref.normalParams && s.exceptional.params == ref.exceptionalParams &&
                s.normal.pointeeParams == ref.normalPointees &&
                s.exceptional.pointeeParams == ref.exceptionalPointees &&
                s.normal.present == ref.anyNormal && s.exceptional.present == ref.anyExceptional,
            "Joint summary: " + name + ": same transformers as one solve per parameter");
    }

    ArgPath pathOf(unsigned argIndex, std::uint64_t offset)
    {
        ArgPath path;
        path.argIndex = argIndex;
        path.offset = offset;
        return path;
    }

    // Parameters 0 and 1 by value in locations 0 and 1; the pointee of argument 2 at offset 0 in
    // location 2, at offset 8 in location 3. Location 4 is a local.
    OwnershipFacts severalParams(std::size_t blocks, std::vector<Edge> edges)
    {
        OwnershipFacts f = facts(blocks, std::move(edges), 5, 1);
        f.paramLocations = {{0, 0}, {1, 1}};
        f.locations[2].kind = LocationKind::ArgPointee;
        f.locations[2].path = pathOf(2, 0);
        f.locations[3].kind = LocationKind::ArgPointee;
        f.locations[3].path = pathOf(2, 8);
        f.pointeeLocations = {{pathOf(2, 0), 2}, {pathOf(2, 8), 3}};
        return f;
    }

    Event unknownCallOn(LocationId loc)
    {
        Event e;
        e.kind = Event::Kind::UnknownCall;
        e.args = {loc};
        return e;
    }

    bool testJointSummaries(TestReport& r)
    {
        {
            // Aliases: parameter 0 is stored into the pointee of argument 2, then released
            // through the local that copies it; parameter 1 goes to an unknown call.
            OwnershipFacts f = severalParams(1, {});
            f.blocks[0].events = {copy(4, 0),        copy(2, 0, /*strong=*/false),
                                  release(4),        unknownCallOn(1),
                                  copy(3, 1, false), exit()};
            expectSameAsReference(r, f, 0, "aliases");
        }
        {
            // Pointee paths: offset 8 is released, offset 0 escapes through its address, and a
            // fresh resource is acquired into the local.
            OwnershipFacts f = severalParams(1, {});
            f.blocks[0].events = {release(3), addressEscape(2), acquire(0, 4), release(4), exit()};
            expectSameAsReference(r, f, 0, "pointee paths");
        }
        {
            // A loop that may release parameter 0 on each turn, overwrites parameter 1, and
            // copies the pointee at offset 0 into the local.
            OwnershipFacts f = severalParams(3, {edge(0, 1), edge(1, 1), edge(1, 2)});
            f.blocks[1].events = {
                callWith(0, StateSet::of(OwnState::Owned) | StateSet::of(OwnState::Released)),
                copy(4, 2, false), overwrite(1, true, false)};
            f.blocks[2].events = {release(4), exit()};
            expectSameAsReference(r, f, 0, "loop");
        }
        {
            // Exceptional exits: an unknown call on parameter 1 may throw before parameter 0
            // is released on the normal path; the pointee at offset 8 is released on both.
            OwnershipFacts f = severalParams(3, {edge(0, 1), edge(0, 2, {release(3), exit(true)})});
            f.blocks[0].events = {unknownCallOn(1)};
            f.blocks[1].events = {release(0), release(3), exit()};
            expectSameAsReference(r, f, 0, "exceptional exits");
        }
        {
            // Convergence budget: below the smallest budget at which one solve per parameter
            // converges, both are incomplete; at that budget, the joint solves converge too.
            OwnershipFacts f = severalParams(4, {edge(0, 1), edge(1, 2), edge(2, 1), edge(2, 3)});
            f.blocks[1].events = {copy(4, 0, false), callWith(1, StateSet::of(OwnState::Released))};
            f.blocks[2].events = {callWith(4, StateSet::of(OwnState::Escaped)), copy(0, 1, false)};
            f.blocks[3].events = {exit()};
            unsigned smallest = 0;
            for (unsigned limit = 1; limit <= 64 && smallest == 0; ++limit)
            {
                if (!referenceTransformers(f, limit).incomplete)
                    smallest = limit;
            }
            r.expect(smallest > 1,
                     "Joint summary: the budget case converges, after more than one visit");
            if (smallest > 1)
            {
                expectSameAsReference(r, f, smallest - 1, "below the smallest converging budget");
                expectSameAsReference(r, f, smallest, "at the smallest converging budget");
            }
        }
        {
            // Fresh resources beside several parameters: one returned, one handed through the
            // pointee of argument 2 at offset 0; parameter 0 is released, parameter 1 escapes.
            OwnershipFacts f = severalParams(1, {});
            f.siteCount = 2;
            f.siteKinds = {"Widget", "Socket"};
            f.locations[4].kind = LocationKind::Return;
            f.blocks[0].events = {release(0),    unknownCallOn(1), acquire(0, 4),
                                  acquire(1, 2), ret(4),           exit()};
            expectSameAsReference(r, f, 0, "fresh resources");
            const FunctionOwnershipSummary s = computeSummary(f);
            r.expect(s.normal.returns.certainty == Certainty::Guaranteed &&
                         s.normal.returns.kind == "Widget",
                     "Joint summary: a fresh resource returned beside several parameters");
            r.expect(s.normal.outArgs.count(pathOf(2, 0)) == 1 &&
                         s.normal.outArgs.at(pathOf(2, 0)).certainty == Certainty::Guaranteed &&
                         s.normal.outArgs.at(pathOf(2, 0)).kind == "Socket" &&
                         s.normal.outArgs.count(pathOf(2, 8)) == 0,
                     "Joint summary: a fresh resource handed through one pointee only");

            // The same acquisitions, conditional.
            f.blocks[0].events = {release(0),
                                  unknownCallOn(1),
                                  acquire(0, 4, true, Certainty::Conditional),
                                  acquire(1, 2, true, Certainty::Conditional),
                                  ret(4),
                                  exit()};
            const FunctionOwnershipSummary c = computeSummary(f);
            r.expect(c.normal.returns.certainty == Certainty::Conditional &&
                         c.normal.outArgs.at(pathOf(2, 0)).certainty == Certainty::Conditional,
                     "Joint summary: conditional fresh resources beside several parameters");
        }
        return r.failures == 0;
    }

    bool testJointSummaryWork(TestReport& r)
    {
        // Six parameters and pointees through a loop: four solves, one per entry state, instead
        // of four per parameter and one more for the fresh resources.
        OwnershipFacts f = severalParams(3, {edge(0, 1), edge(1, 1), edge(1, 2)});
        f.paramLocations.push_back({3, 4});
        f.locations.resize(6);
        f.locations[5].kind = LocationKind::ArgPointee;
        f.locations[5].path = pathOf(4, 0);
        f.pointeeLocations.push_back({pathOf(4, 0), 5});
        f.blocks[1].events = {callWith(0, StateSet::of(OwnState::Released)), copy(2, 1, false)};
        f.blocks[2].events = {exit()};
        expectSameAsReference(r, f, 0, "six parameters and pointees");

        SummaryWork work;
        (void)computeSummary(f, 0, &work);
        const OwnershipResult allOwned =
            solve(f, summaryEntry(f, f.paramLocations.size() + f.pointeeLocations.size(),
                                  OwnState::Owned));
        r.expect(work.solves == 4, "Joint summary: four solves for six parameters and pointees (" +
                                       std::to_string(work.solves) + ")");
        r.expect(allOwned.blockVisits > 0 && work.blockVisits <= 4 * allOwned.blockVisits,
                 "Joint summary: at most four times the block visits of one solve (" +
                     std::to_string(work.blockVisits) + " for " +
                     std::to_string(allOwned.blockVisits) + " in one solve)");

        OwnershipFacts none = facts(1, {}, 1, 1);
        none.blocks[0].events = {acquire(0, 0), release(0), exit()};
        SummaryWork noneWork;
        (void)computeSummary(none, 0, &noneWork);
        r.expect(noneWork.solves == 1, "Joint summary: one solve without parameters (" +
                                           std::to_string(noneWork.solves) + ")");
        return r.failures == 0;
    }
    // ---- summaries of functions that consult each other's -------------------------------------

    // A small integer carried by a summary, so that a synthetic computation can depend on the
    // summaries it consults.
    int valueOf(const FunctionOwnershipSummary& s)
    {
        return s.normal.returns.kind.empty() ? 0 : std::stoi(s.normal.returns.kind);
    }
    FunctionOwnershipSummary withValue(int value, bool incomplete = false)
    {
        FunctionOwnershipSummary s;
        s.normal.returns.kind = std::to_string(value);
        s.incomplete = incomplete;
        return s;
    }

    using Compute = std::function<FunctionOwnershipSummary(std::size_t, const ConsultSummary&)>;

    // The fixed point as computeOwnershipSummaries ran it before it reused summaries: every
    // function recomputed in index order each round until a round changes none; past maxRounds
    // rounds, every summary is incomplete.
    std::vector<FunctionOwnershipSummary> referenceFixpoint(std::size_t count,
                                                            const Compute& compute,
                                                            unsigned maxRounds,
                                                            std::uint64_t& computations)
    {
        std::vector<FunctionOwnershipSummary> summaries(count);
        const ConsultSummary consult = [&](std::size_t j) -> const FunctionOwnershipSummary&
        { return summaries[j]; };
        for (unsigned round = 0; round < maxRounds; ++round)
        {
            bool changed = false;
            for (std::size_t i = 0; i < count; ++i)
            {
                FunctionOwnershipSummary next = compute(i, consult);
                ++computations;
                if (!sameSummary(summaries[i], next))
                {
                    summaries[i] = std::move(next);
                    changed = true;
                }
            }
            if (!changed)
                return summaries;
        }
        for (FunctionOwnershipSummary& s : summaries)
            s.incomplete = true;
        return summaries;
    }

    // computeSummaryFixpoint gives what the reference gives, with fewer computations or as many.
    // Returns its work.
    SummaryFixpointWork expectSameFixpoint(TestReport& r, std::size_t count, const Compute& compute,
                                           unsigned maxRounds, const std::string& name)
    {
        std::uint64_t referenceComputations = 0;
        const auto expected = referenceFixpoint(count, compute, maxRounds, referenceComputations);
        SummaryFixpointWork work;
        const auto got = computeSummaryFixpoint(count, compute, maxRounds, &work);
        bool same = got.size() == expected.size();
        for (std::size_t i = 0; same && i < got.size(); ++i)
            same = sameSummary(got[i], expected[i]);
        r.expect(same,
                 "Summary fixpoint: " + name + ": the summaries of recomputing every function");
        r.expect(work.computations <= referenceComputations,
                 "Summary fixpoint: " + name + ": no more computations than recomputing all (" +
                     std::to_string(work.computations) + " for " +
                     std::to_string(referenceComputations) + ")");
        return work;
    }

    bool testSummaryFixpoint(TestReport& r)
    {
        {
            // Independent functions: one round computes them, the next changes nothing; none
            // of them consulted a summary, so none is computed twice.
            const Compute compute = [](std::size_t i, const ConsultSummary&)
            { return withValue(static_cast<int>(i)); };
            const SummaryFixpointWork work = expectSameFixpoint(r, 5, compute, 16, "independent");
            r.expect(work.computations == 5 && work.rounds == 2,
                     "Summary fixpoint: independent functions are computed once (" +
                         std::to_string(work.computations) + " computations, " +
                         std::to_string(work.rounds) + " rounds)");
        }
        {
            // A chain against the index order: function i consults function i + 1, which a round
            // only reaches after it, so the value moves one function per round. A function is
            // computed again only once the one it consults has changed.
            const Compute compute = [](std::size_t i, const ConsultSummary& consult)
            { return withValue(i == 5 ? 1 : valueOf(consult(i + 1)) + 1); };
            const SummaryFixpointWork work = expectSameFixpoint(r, 6, compute, 16, "chain");
            r.expect(work.computations < 6 * work.rounds,
                     "Summary fixpoint: a chain recomputes fewer than every function each round (" +
                         std::to_string(work.computations) + " computations, " +
                         std::to_string(work.rounds) + " rounds)");
        }
        {
            // Recursion: a function that consults itself until it reaches 3, and one that copies it.
            const Compute compute = [](std::size_t i, const ConsultSummary& consult)
            {
                return i == 0 ? withValue(std::min(valueOf(consult(0)) + 1, 3))
                              : withValue(valueOf(consult(0)));
            };
            expectSameFixpoint(r, 2, compute, 16, "recursion");
        }
        {
            // Mutual recursion: each consults the other.
            const Compute compute = [](std::size_t i, const ConsultSummary& consult)
            {
                return i == 0 ? withValue(std::min(valueOf(consult(1)) + 1, 4))
                              : withValue(valueOf(consult(0)));
            };
            expectSameFixpoint(r, 2, compute, 16, "mutual recursion");
        }
        {
            // Only the incomplete status changes: function 1 becomes incomplete, its value kept,
            // once function 2 reaches 2; function 0 copies the status of function 1.
            const Compute compute = [](std::size_t i, const ConsultSummary& consult)
            {
                if (i == 2)
                    return withValue(std::min(valueOf(consult(2)) + 1, 2));
                if (i == 1)
                    return withValue(7, valueOf(consult(2)) >= 2);
                return withValue(0, consult(1).incomplete);
            };
            std::uint64_t ignored = 0;
            const auto expected = referenceFixpoint(3, compute, 16, ignored);
            r.expect(expected[0].incomplete,
                     "Summary fixpoint: the incomplete case does reach function 0");
            expectSameFixpoint(r, 3, compute, 16, "incomplete status");
        }
        {
            // A summary consulted, then changed later in the same round: function 0 consults
            // function 1, which a round reaches after it, and keeps its initial summary until
            // function 1 reaches 2. Its computations leave its summary unchanged until then, and
            // the change of function 1 comes after them in each round: function 0 must still be
            // computed again.
            const Compute compute = [](std::size_t i, const ConsultSummary& consult)
            {
                if (i == 1)
                    return withValue(std::min(valueOf(consult(1)) + 1, 2));
                return valueOf(consult(1)) >= 2 ? withValue(20) : FunctionOwnershipSummary{};
            };
            std::uint64_t ignored = 0;
            r.expect(valueOf(referenceFixpoint(2, compute, 16, ignored)[0]) == 20,
                     "Summary fixpoint: the changed-input case does reach function 0");
            expectSameFixpoint(r, 2, compute, 16, "input changed later in the round");
        }
        {
            // No fixed point within the budget: every summary ends incomplete, after as many
            // rounds as the budget allows.
            const Compute compute = [](std::size_t i, const ConsultSummary& consult)
            { return i == 0 ? withValue(valueOf(consult(0)) + 1) : withValue(5); };
            const SummaryFixpointWork work = expectSameFixpoint(r, 2, compute, 16, "budget");
            r.expect(work.rounds == 16, "Summary fixpoint: the budget bounds the rounds (" +
                                            std::to_string(work.rounds) + ")");
        }
        return r.failures == 0;
    }
} // namespace

int main(int, char**)
{
    TestReport report;
    (void)testDomain(report);
    (void)testEngine(report);
    (void)testContracts(report);
    (void)testEdgeExits(report);
    (void)testExitUncertainty(report);
    (void)testSummaries(report);
    (void)testSummaryMetadata(report);
    (void)testJointSummaries(report);
    (void)testJointSummaryWork(report);
    (void)testSummaryFixpoint(report);
    if (report.failures == 0)
    {
        std::cout << "All ownership engine unit tests passed.\n";
        return 0;
    }
    std::cerr << report.failures << " ownership engine unit test(s) failed.\n";
    return 1;
}
