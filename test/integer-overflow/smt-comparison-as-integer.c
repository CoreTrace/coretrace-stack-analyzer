// SPDX-License-Identifier: Apache-2.0
//
// SMT encoding: a comparison used as an integer.
//
// For the solver a comparison is a boolean; C turns it into an int, 0 or 1, as soon as it is
// stored or added. The encoding must convert it, or no query that reads it can be built:
// assert-enabled builds abort in Z3 (bv_size on a boolean), release builds lose the query.

#include <stdbool.h>

// Where `less` is set, a < b holds, so `a + 1` cannot overflow.
int stored_flag(int a, int b)
{
    int less = a < b;
    if (less)
        return a + 1;
    return 0;
}

// The same flag through a bool.
int bool_flag(int a, int b)
{
    bool less = a < b;
    if (less)
        return a + 1;
    return 0;
}

// The comparison in the arithmetic itself: 0 or 1, plus 1.
int direct_flag(int a, int b)
{
    return (a < b) + 1;
}

// [default] at line 16, column 18
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: add
// ↳ result is returned without a provable non-overflow bound

// [default] at line 25, column 18
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: add
// ↳ result is returned without a provable non-overflow bound

// [default] at line 32, column 20
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: add
// ↳ result is returned without a provable non-overflow bound

// [smt-z3] not contains: potential signed integer overflow in arithmetic operation
