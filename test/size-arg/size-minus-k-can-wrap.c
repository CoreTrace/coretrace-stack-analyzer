// SPDX-License-Identifier: Apache-2.0
//
// SizeMinusOneWrite: `s - k` wraps when it can fall below the minimum of its type (#152): 0 for
// an unsigned size, INT_MIN for an int. All of these stay reported.

#include <stddef.h>
#include <string.h>

// n may be 0: buf[n - 1] writes before buf.
void terminate_unchecked(char* buf, size_t n)
{
    buf[n - 1] = '\0';
}

// n may be 0: the length wraps to SIZE_MAX.
void copy_short(char* dst, const char* src, size_t n)
{
    if (n < 100)
        strncpy(dst, src, n - 1);
}

// n is not 0, but may be INT_MIN: n - 1 overflows.
void terminate_signed(char* buf, int n)
{
    if (n != 0)
        buf[n - 1] = '\0';
}

// The same for a length: n - 1 overflows at INT_MIN. A negative n - 1, converted to a huge
// size_t, is another defect, which IntegerConversion.SignedToSize reports (CWE-195).
void copy_signed(char* dst, const char* src, int n)
{
    if (n != 0)
        strncpy(dst, src, n - 1);
}

// at line 12, column 16
// [ !!Warn ] potential unsafe write with length (size - 1) in store (idx = size-k)
// ↳ size operand may be less than 1

// at line 19, column 9
// [ !!Warn ] potential unsafe write with length (size - 1) in strncpy
// ↳ size operand may be less than 1

// at line 26, column 20
// [ !!Warn ] potential unsafe write with length (size - 1) in store (idx = size-k)
// ↳ signed size - 1 may fall below the minimum of its type

// at line 34, column 9
// [ !!Warn ] potential unsafe write with length (size - 1) in strncpy
// ↳ signed size - 1 may fall below the minimum of its type

// at line 34, column 9
// [ !!Warn ] potential signed-to-size conversion before 'strncpy'
// ↳ a possibly negative signed value is converted to an unsigned length
// ↳ this can become a very large size value and trigger out-of-bounds access

// strict-expectation-details: true
