// SPDX-License-Identifier: Apache-2.0
//
// SMT path conditions: the CERT INT32-C precondition makes `a + b` safe.
//
// The guard is a disjunction compiled to several branches, and it relates `a` to `b`, so no
// interval on either operand proves the addition safe. The solver proves it from the
// reachability condition of the return, which it only sees once loads of the same slot are
// tied together.

#include <limits.h>

int checked_add(int a, int b)
{
    if ((b > 0 && a > INT_MAX - b) || (b < 0 && a < INT_MIN - b))
        return -1;
    return a + b;
}

// [default] at line 16, column 14
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: add
// ↳ result is returned without a provable non-overflow bound

// [smt-z3] not contains: potential signed integer overflow in arithmetic operation
