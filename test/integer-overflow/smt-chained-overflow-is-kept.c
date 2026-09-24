// SPDX-License-Identifier: Apache-2.0
//
// Guard: the SMT encoder must not assume `nsw` on the operands of the queried operation.
//
// Only the `sub` is reported: reachesReturn() does not follow arithmetic, so the `add`, which
// overflows for a == INT_MAX, is never checked on its own. Assuming the `add` cannot wrap
// would let the solver discharge the `sub` and hide the only report of this overflow.
//
// This fixture must report in BOTH passes.

int add_then_subtract(int a)
{
    return (a + 1) - 1;
}

// strict-expectation-details: true

// at line 13, column 20
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: sub
// ↳ result is returned without a provable non-overflow bound
