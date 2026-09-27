// SPDX-License-Identifier: Apache-2.0
//
// SizeMinusOneWrite: `s - k` cannot wrap when `s >= k` holds, nor when a signed `s - k` is only
// negative, and a destination that may be null is not a size-minus-k defect (#152). None of
// these writes may be reported.

#include <limits.h>
#include <stddef.h>
#include <string.h>

// n > 0: n - 1 is a valid index.
void terminate(char* buf, size_t n)
{
    if (n > 0)
        buf[n - 1] = '\0';
}

// n >= 2: the length n - 1 is at least 1.
void copy_prefix(char* dst, const char* src, size_t n)
{
    if (n >= 2)
        strncpy(dst, src, n - 1);
}

// n is 8: only the destination is in question.
void copy_seven(char* dst, const char* src)
{
    size_t n = 8;
    strncpy(dst, src, n - 1);
}

// The same guard on an unsigned int, zero-extended into the index.
void terminate_unsigned(char* buf, unsigned n)
{
    if (n > 0)
        buf[n - 1] = '\0';
}

// n > INT_MIN: n - 1 cannot overflow. It may be negative, and the write then lands before buf: a
// buffer underwrite (CWE-124, #154), not an underflow of the subtraction.
void terminate_negative(char* buf, int n)
{
    if (n > INT_MIN)
        buf[n - 1] = '\0';
}

// The same for a length: strncpy converts a negative n - 1 to a huge size_t, a signed to
// unsigned conversion error (CWE-195), not an underflow of the subtraction.
void copy_negative(char* dst, const char* src, int n)
{
    if (n > INT_MIN)
        strncpy(dst, src, n - 1);
}

// not contains: potential unsafe write with length (size - 1)
