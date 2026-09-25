// SPDX-License-Identifier: Apache-2.0
//
// SMT refinement asks the solver even when no range is known.
//
// `l * sizeof(char)` multiplies by one and cannot wrap. With no range for `l`, the default
// pass reports the size computation; the solver proves it safe without any range.

#include <string.h>

void copy_chars(char* dst, const char* src, unsigned long l)
{
    memcpy(dst, src, l * sizeof(char));
}

// [default] at line 12, column 5
// [ !!Warn ] potential integer overflow in size computation before 'memcpy'
// ↳ operation: mul
// ↳ overflowed size may under-allocate memory or make bounds checks unsound

// [smt-z3] not contains: potential integer overflow in size computation
