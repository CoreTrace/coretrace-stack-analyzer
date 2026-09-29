// SPDX-License-Identifier: Apache-2.0
//
// IntegerConversion.SignedToSize (#156): strncpy, strncat and stpncpy take a size_t length, as
// memcpy does. A signed length that may be negative becomes a huge size (CWE-195). n > INT_MIN
// rules out an overflow of n - 1: SizeMinusOneWrite has nothing to say here.

#include <limits.h>
#include <string.h>

void strncpy_negative(char* dst, const char* src, int n)
{
    if (n > INT_MIN)
        strncpy(dst, src, n - 1);
}

void strncat_negative(char* dst, const char* src, int n)
{
    if (n > INT_MIN)
        strncat(dst, src, n - 1);
}

void stpncpy_negative(char* dst, const char* src, int n)
{
    if (n > INT_MIN)
        stpncpy(dst, src, n - 1);
}

void strncpy_param(char* dst, const char* src, int len)
{
    strncpy(dst, src, len);
}

// at line 13, column 9
// [ !!Warn ] potential signed-to-size conversion before 'strncpy'
// ↳ a possibly negative signed value is converted to an unsigned length
// ↳ this can become a very large size value and trigger out-of-bounds access

// at line 19, column 9
// [ !!Warn ] potential signed-to-size conversion before 'strncat'
// ↳ a possibly negative signed value is converted to an unsigned length
// ↳ this can become a very large size value and trigger out-of-bounds access

// at line 25, column 9
// [ !!Warn ] potential signed-to-size conversion before 'stpncpy'
// ↳ a possibly negative signed value is converted to an unsigned length
// ↳ this can become a very large size value and trigger out-of-bounds access

// at line 30, column 5
// [ !!Warn ] potential signed-to-size conversion before 'strncpy'
// ↳ a possibly negative signed value is converted to an unsigned length
// ↳ this can become a very large size value and trigger out-of-bounds access

// strict-expectation-details: true
