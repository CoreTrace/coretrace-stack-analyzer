// SPDX-License-Identifier: Apache-2.0
//
// A static check() that returns (#153), while cross-tu-noreturn-static-a.c has one that never
// returns: its guard protects nothing, whatever the other file.

#include <stddef.h>
#include <string.h>

static int check(const char* message)
{
    (void)message;
    return 0;
}

void copy_b(char* dst, const char* src, size_t n)
{
    if (n < 1)
        check("empty");
    strncpy(dst, src, n - 1);
}

// at line 19, column 5
// [ !!Warn ] potential unsafe write with length (size - 1) in strncpy
// ↳ size operand may be less than 1

// strict-expectation-details: true
