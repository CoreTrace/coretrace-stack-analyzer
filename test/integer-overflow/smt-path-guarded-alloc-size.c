// SPDX-License-Identifier: Apache-2.0
//
// SMT path conditions: the guard on `n` makes `n * 8` unable to wrap before `malloc`.

#include <stdint.h>
#include <stdlib.h>

void* allocate_words(size_t n)
{
    if (n > SIZE_MAX / 8)
        return 0;
    return malloc(n * 8);
}

// [default] at line 12, column 12
// [ !!Warn ] potential integer overflow in size computation before 'malloc'
// ↳ operation: mul
// ↳ overflowed size may under-allocate memory or make bounds checks unsound

// [smt-z3] not contains: potential integer overflow in size computation
