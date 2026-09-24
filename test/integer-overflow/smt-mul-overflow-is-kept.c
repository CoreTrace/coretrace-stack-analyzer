// SPDX-License-Identifier: Apache-2.0
//
// Guard: the overflow query of a multiplication must be exact.
//
// Widening both operands by one bit is enough to compute a sum or a difference exactly, but not
// a product: the widened product can itself wrap and look like no overflow. Each product below
// overflows for some input (x odd, or big != 0), so this fixture must report all three, in BOTH
// passes.

#include <string.h>

long signed_product(unsigned x)
{
    return (long)(int)(x << 31) * (1L << 34);
}

void size_product(char* dst, const char* src, unsigned x)
{
    memcpy(dst, src, (size_t)(x << 31) * (1UL << 34));
}

int selected_product(int big)
{
    return (big ? 200 : 10) * 33554432;
}

// at line 14, column 33
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: mul
// ↳ result is returned without a provable non-overflow bound

// at line 19, column 5
// [ !!Warn ] potential integer overflow in size computation before 'memcpy'
// ↳ operation: mul
// ↳ overflowed size may under-allocate memory or make bounds checks unsound

// at line 24, column 29
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: mul
// ↳ result is returned without a provable non-overflow bound
