// SPDX-License-Identifier: Apache-2.0
//
// A my_copy() that does not pass n as a length (#157). Beside the definition of
// cross-tu-size-minus-one-def.c, the symbol has no pair: only the pairs of every definition hold.

#include <string.h>

void my_copy(char* dst, const char* src, size_t n)
{
    (void)n;
    strncpy(dst, src, 8);
}
