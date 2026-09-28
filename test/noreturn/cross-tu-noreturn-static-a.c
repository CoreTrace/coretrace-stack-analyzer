// SPDX-License-Identifier: Apache-2.0
//
// A static check() that never returns (#153). cross-tu-noreturn-static-b.c has a static check()
// that returns: analyzed together, each file keeps its own.

#include <stddef.h>
#include <string.h>

__attribute__((noreturn)) void raise_error(const char* message);

static int check(const char* message)
{
    raise_error(message);
    return 0;
}

// n >= 1 below the guard: n - 1 cannot wrap.
void copy_a(char* dst, const char* src, size_t n)
{
    if (n < 1)
        check("empty");
    strncpy(dst, src, n - 1);
}

// not contains: potential unsafe write with length (size - 1)
