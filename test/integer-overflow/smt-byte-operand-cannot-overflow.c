// SPDX-License-Identifier: Apache-2.0
//
// SMT refinement asks the solver even when no range is known.
//
// `1 + c->n` widens an unsigned char before adding, so the sum is at most 256 and cannot
// overflow an int. The range analysis has no bound for a byte loaded from memory, so the
// default pass reports the addition; the solver proves it safe from the bit widths alone.

struct counter
{
    unsigned char n;
};

int byte_plus_one(const struct counter* c)
{
    return 1 + c->n;
}

// [default] at line 16, column 14
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: add
// ↳ result is returned without a provable non-overflow bound

// [smt-z3] not contains: potential signed integer overflow in arithmetic operation
