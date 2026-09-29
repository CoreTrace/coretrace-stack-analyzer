// SPDX-License-Identifier: Apache-2.0
//
// A static fail() that returns (#170). It belongs to this file only: it is not a definition of
// the symbol fail() that cross-tu-noreturn-conflict-use.c calls, and must not keep the one of
// cross-tu-noreturn-conflict-exits.c from counting.

#include <stdio.h>

static void fail(const char* message)
{
    fputs(message, stderr);
}

void report(const char* message)
{
    fail(message);
}
