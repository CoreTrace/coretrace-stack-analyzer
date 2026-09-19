// SPDX-License-Identifier: Apache-2.0
// Unit tests for the LLVM-free ownership engine (docs/superpowers/specs/
// 2026-09-19-resource-ownership-engine-design.md). Facts are built by hand.
#include "analysis/ownership/OwnershipDomain.hpp"

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
        r.expect(s.has(OwnState::Owned) && s.has(OwnState::Released) &&
                     !s.isOnly(OwnState::Owned),
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
        r.expect(d.resources.size() == 2 && d.resources[0] == 1 && d.resources[1] == 3 &&
                     d.mayNull,
                 "Domain: merge keeps contents sorted and unique");
        return r.failures == 0;
    }
} // namespace

int main(int, char**)
{
    TestReport report;
    (void)testDomain(report);
    if (report.failures == 0)
    {
        std::cout << "All ownership engine unit tests passed.\n";
        return 0;
    }
    std::cerr << report.failures << " ownership engine unit test(s) failed.\n";
    return 1;
}
