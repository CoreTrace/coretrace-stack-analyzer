// SPDX-License-Identifier: Apache-2.0
//
// Calls my_copy() through a declaration without a prototype (#157). With
// cross-tu-size-minus-one-def.c, the first call passes one argument per parameter: its length
// n - 1 reaches strncpy, as within one file. The second passes one argument too many, and the
// pair does not apply to it.

#include <stddef.h>

void my_copy();

void copy_noproto(char* dst, const char* src, size_t n)
{
    my_copy(dst, src, n - 1);
}

void copy_noproto_long(char* dst, const char* src, size_t n)
{
    my_copy(dst, src, n - 1, 0);
}
