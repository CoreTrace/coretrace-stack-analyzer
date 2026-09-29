// SPDX-License-Identifier: Apache-2.0
//
// IntegerConversion.SignedToSize (#156): copy_bytes has no body here, but the buffer model
// declares its third argument the length it writes. A length that may be negative is then
// reported for it too.

#include <limits.h>
#include <stddef.h>

void copy_bytes(char* dst, const char* src, size_t n);

void model_negative(char* dst, const char* src, int n)
{
    if (n > INT_MIN)
        copy_bytes(dst, src, n - 1);
}

// at line 15, column 9
// [ !!Warn ] potential signed-to-size conversion before 'copy_bytes'
// ↳ a possibly negative signed value is converted to an unsigned length
// ↳ this can become a very large size value and trigger out-of-bounds access

// buffer-model: test/integer-overflow/signed-length-model.txt
// strict-expectation-details: true
