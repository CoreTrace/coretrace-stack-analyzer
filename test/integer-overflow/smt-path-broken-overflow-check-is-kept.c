// SPDX-License-Identifier: Apache-2.0
//
// Guard: path conditions use the wrapping arithmetic that -O0 code executes.
//
// `a + 1 < a` only holds when the addition wraps (a == INT_MAX), and then `a + 2` overflows.
// Assuming `nsw` on the condition would make the branch look unreachable and hide the report.
//
// This fixture must report in BOTH passes.

int broken_check(int a)
{
    if (a + 1 < a)
        return a + 2;
    return 0;
}

// strict-expectation-details: true

// at line 13, column 18
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: add
// ↳ result is returned without a provable non-overflow bound
