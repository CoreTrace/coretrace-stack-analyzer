// SPDX-License-Identifier: Apache-2.0
//
// Guards IntRanges.cpp::restsOnWrapAssumption().
//
// `v + 1` on a signed int is emitted as `add nsw`. LLVM derives the range of an `add nsw`
// from the promise that it does not wrap -- which is exactly the property this analysis is
// trying to establish. If that range is published into the shared IntRange map, the SMT pass
// receives the assumption it was supposed to verify and proves the overflow infeasible, so
// the warning below silently disappears in the smt-z3 pass while still appearing in the
// default pass.
//
// This fixture must report in BOTH passes. If only the smt-z3 pass loses it, the exclusion
// of wrap-flagged values from the range map has been dropped.

int add_one_to_argument(int v)
{
    return v + 1;
}

// strict-expectation-details: true

// at line 17, column 14
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: add
// ↳ result is returned without a provable non-overflow bound
