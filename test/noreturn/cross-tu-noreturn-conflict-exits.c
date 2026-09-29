// SPDX-License-Identifier: Apache-2.0
//
// One of the definitions of fail() for cross-tu-noreturn-conflict-use.c (#170): this one never
// returns. cross-tu-noreturn-conflict-returns.c defines the same symbol and returns. A caller may
// be linked with either, so with both files fail() may return.

#include <stdlib.h>

void fail(const char* message)
{
    (void)message;
    exit(1);
}
