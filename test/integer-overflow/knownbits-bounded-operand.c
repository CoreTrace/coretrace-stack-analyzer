// SPDX-License-Identifier: Apache-2.0
//
// End-to-end guard for the KnownBits-derived bounds published by computeIntRanges().
//
// 255 + 1 cannot overflow an int, but nothing in the IR compares `masked` against anything,
// so the legacy comparison scan has nothing to say and the analyzer used to report a signed
// overflow here. Discharging it requires a range that only ValueTracking can prove.
//
// The bound reaches the addition by two independent routes -- the per-instruction
// publication in computeIntRanges() and publishSingleStoreSlots() -- and either one alone is
// enough, so this fixture only turns red when the range publication is removed wholesale. It
// is the end-to-end witness that the feature is wired up at all; the individual mechanisms
// are pinned by testIntRangeFacts in test/unit/analyzer_module_unit_tests.cpp.

int add_one_to_masked(int seed)
{
    int masked = seed & 0xFF;
    return masked + 1;
}

// not contains: potential signed integer overflow in arithmetic operation
