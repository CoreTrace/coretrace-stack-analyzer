// SPDX-License-Identifier: Apache-2.0
//
// An indirect caller of fail() (#170): must_fail() of cross-tu-noreturn-conflict-use.c never
// returns only if fail() never returns, and so does the guard below.

#include <stddef.h>
#include <string.h>

void must_fail(void);

void copy_twice_removed(char* dst, const char* src, size_t n)
{
    if (n < 1)
        must_fail();
    strncpy(dst, src, n - 1);
}

// at line 15, column 5
// [ !!Warn ] potential unsafe write with length (size - 1) in strncpy
// ↳ size operand may be less than 1

// strict-expectation-details: true
