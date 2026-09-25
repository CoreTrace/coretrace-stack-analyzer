// SPDX-License-Identifier: Apache-2.0
//
// Guard: a defect in code that never runs stays reported, as the default pass reports it.
//
// `limit` is 5, so `limit > 10` never holds and `a + 1` never executes. The path condition of
// the addition is unsatisfiable, which makes every query there infeasible: the solver must not
// take that for a proof that the addition cannot overflow.
//
// This fixture must report in BOTH passes.

int dead_add(int a)
{
    int limit = 5;
    if (limit > 10)
        return a + 1;
    return 0;
}

// at line 15, column 18
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: add
// ↳ result is returned without a provable non-overflow bound
