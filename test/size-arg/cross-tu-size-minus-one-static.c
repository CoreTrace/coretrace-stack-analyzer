// SPDX-License-Identifier: Apache-2.0
//
// A static my_copy() (#157). It belongs to this file only: it gives nothing to the my_copy() that
// cross-tu-size-minus-one-use.c calls, and it does not count as one of its definitions. Within
// this file it passes n to strncpy: local_copy() is reported.

#include <string.h>

static void my_copy(char* dst, const char* src, size_t n)
{
    strncpy(dst, src, n);
}

void local_copy(char* dst, const char* src, size_t n)
{
    my_copy(dst, src, n - 1);
}

// at line 16, column 5
// [ !!Warn ] potential unsafe write with length (size - 1) in my_copy
//          ↳ size operand may be less than 1
