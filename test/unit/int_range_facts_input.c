// SPDX-License-Identifier: Apache-2.0
//
// Input for testIntRangeFacts in analyzer_module_unit_tests.cpp. It lives under test/unit/,
// which run_test.py excludes from fixture discovery, because the properties it pins are
// about the contents of the IntRange map and are not observable through any diagnostic.
//
// No comparison against a constant appears anywhere below, so every range the test observes
// must come from FunctionFacts rather than from the legacy ICmp scan.

unsigned long int_range_facts_input(int seed, int flag)
{
    // Single-assignment slot: KnownBits bounds this to [0, 255] and the bridge must mirror
    // that onto the slot and its loads.
    int masked = seed & 255;

    // Multiply-assigned slot: the two stores hold different ranges, so no range at all may
    // be published for it. This is the case no diagnostic can expose -- both the overflow
    // and the out-of-bounds detectors refuse to reason through a rewritten slot before the
    // range map is ever consulted, so an unsound bridge would go unnoticed end to end.
    int rewritten = seed & 15;
    if (flag)
        rewritten = seed;

    // Wrap-flagged arithmetic: LLVM derives its range from the nsw promise, which is the
    // property IntegerOverflowAnalysis exists to check. Neither link of the chain may be
    // published.
    int sum = masked + 1;
    int chained = sum + 1;

    // zext i32 -> i64: "at most 4294967295" restates the source width and bounds nothing.
    unsigned long widened = (unsigned long)(unsigned)seed;

    return (unsigned long)(masked + rewritten + chained) + widened;
}
