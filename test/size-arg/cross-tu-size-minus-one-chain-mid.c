// SPDX-License-Identifier: Apache-2.0
//
// The middle of a chain over three files (#157). mid_copy() has the pair (dst, n) only if
// leaf_copy(), defined in cross-tu-size-minus-one-chain-leaf.c, has it.

#include <stddef.h>

void leaf_copy(char* dst, const char* src, size_t n);

void mid_copy(char* dst, const char* src, size_t n)
{
    leaf_copy(dst, src, n);
}
