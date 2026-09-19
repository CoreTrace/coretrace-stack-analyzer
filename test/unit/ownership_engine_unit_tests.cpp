// SPDX-License-Identifier: Apache-2.0
// Unit tests for the LLVM-free ownership engine (docs/superpowers/specs/
// 2026-09-19-resource-ownership-engine-design.md). Facts are built by hand.
#include "analysis/ownership/OwnershipDomain.hpp"
#include "analysis/ownership/OwnershipEngine.hpp"
#include "analysis/ownership/OwnershipFacts.hpp"

#include <iostream>
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
    if (report.failures == 0)
    {
        std::cout << "All ownership engine unit tests passed.\n";
        return 0;
    }
    std::cerr << report.failures << " ownership engine unit test(s) failed.\n";
    return 1;
}
