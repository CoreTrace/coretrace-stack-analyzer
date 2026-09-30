// SPDX-License-Identifier: Apache-2.0
//
// my_copy() passes its length n to strncpy (#157), for the callers of
// cross-tu-size-minus-one-use.c and cross-tu-size-minus-one-noproto-use.c.

#include <string.h>

void my_copy(char* dst, const char* src, size_t n)
{
    strncpy(dst, src, n);
}
