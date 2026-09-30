// SPDX-License-Identifier: Apache-2.0
//
// Caller of my_copy() (#157). Alone, my_copy() is a declaration without its definition, and
// nothing is reported. With cross-tu-size-minus-one-def.c, my_copy() passes n to strncpy: the
// length n - 1 may wrap, as within one file. run_test.py checks each pairing.

#include <stddef.h>

void my_copy(char* dst, const char* src, size_t n);

void copy_name(char* dst, const char* src, size_t n)
{
    my_copy(dst, src, n - 1);
}
