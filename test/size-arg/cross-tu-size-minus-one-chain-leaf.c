// SPDX-License-Identifier: Apache-2.0
//
// The end of a chain over three files (#157): cross-tu-size-minus-one-chain-use.c calls
// mid_copy(), which calls leaf_copy(), which passes n to strncpy.

#include <string.h>

void leaf_copy(char* dst, const char* src, size_t n)
{
    strncpy(dst, src, n);
}
