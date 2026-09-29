// SPDX-License-Identifier: Apache-2.0
//
// IntegerConversion.SignedToSize (#156): copy_n passes its length on to strncpy, so a call to
// copy_n with a length that may be negative is the same defect as the call to strncpy.

#include <limits.h>
#include <stddef.h>
#include <string.h>

static void copy_n(char* dst, const char* src, size_t n)
{
    strncpy(dst, src, n);
}

void wrapper_negative(char* dst, const char* src, int n)
{
    if (n > INT_MIN)
        copy_n(dst, src, n - 1);
}

// at line 18, column 9
// [ !!Warn ] potential signed-to-size conversion before 'copy_n'
// ↳ a possibly negative signed value is converted to an unsigned length
// ↳ this can become a very large size value and trigger out-of-bounds access

// strict-expectation-details: true
