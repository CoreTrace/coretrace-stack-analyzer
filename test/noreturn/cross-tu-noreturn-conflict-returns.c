// SPDX-License-Identifier: Apache-2.0
//
// Another definition of fail() for cross-tu-noreturn-conflict-use.c (#170): this one returns.

#include <stdio.h>

void fail(const char* message)
{
    fputs(message, stderr);
}
