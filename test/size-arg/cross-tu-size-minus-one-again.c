// SPDX-License-Identifier: Apache-2.0
//
// A second my_copy() that passes n as the length of a copy (#157), as when two programs are
// analyzed together. Every definition of the symbol has the pair (dst, n): the callers of
// cross-tu-size-minus-one-use.c keep it.

#include <string.h>

void my_copy(char* dst, const char* src, size_t n)
{
    memcpy(dst, src, n);
}
