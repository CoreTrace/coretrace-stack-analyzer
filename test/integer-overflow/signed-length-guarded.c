// SPDX-License-Identifier: Apache-2.0
//
// Sizes computed under a guard (#156): under n > 0, n - 1 is at least 0. It can neither be
// negative (IntegerConversion.SignedToSize) nor wrap (IntegerOverflow.SizeComputation),
// whatever the function that receives it. None may be reported, in either pass.

#include <stddef.h>
#include <string.h>

static void copy_n(char* dst, const char* src, size_t n)
{
    strncpy(dst, src, n);
}

void memcpy_guarded(char* dst, const char* src, int n)
{
    if (n > 0)
        memcpy(dst, src, n - 1);
}

void strncpy_guarded(char* dst, const char* src, int n)
{
    if (n > 0)
        strncpy(dst, src, n - 1);
}

void wrapper_guarded(char* dst, const char* src, int n)
{
    if (n > 0)
        copy_n(dst, src, n - 1);
}

// not contains: potential signed-to-size conversion
// not contains: potential integer overflow in size computation
