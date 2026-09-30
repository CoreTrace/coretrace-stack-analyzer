// SPDX-License-Identifier: Apache-2.0
//
// The caller of a chain over three files (#157). With cross-tu-size-minus-one-chain-mid.c and
// cross-tu-size-minus-one-chain-leaf.c, the length n - 1 reaches strncpy. Without the leaf,
// mid_copy() calls an unknown function and nothing is reported.

#include <stddef.h>

void mid_copy(char* dst, const char* src, size_t n);

void copy_chain(char* dst, const char* src, size_t n)
{
    mid_copy(dst, src, n - 1);
}
