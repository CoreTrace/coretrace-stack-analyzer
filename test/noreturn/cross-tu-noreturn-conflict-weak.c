// SPDX-License-Identifier: Apache-2.0
//
// A weak definition of fail() for cross-tu-noreturn-conflict-use.c (#170). It never returns, but
// the linker may retain another definition of the symbol instead: it proves nothing about the
// one that runs.

#include <stdlib.h>

__attribute__((weak)) void fail(const char* message)
{
    (void)message;
    exit(1);
}
