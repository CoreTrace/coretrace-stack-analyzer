// SPDX-License-Identifier: Apache-2.0
//
// Guard: the SMT query must bound the index the access really uses, casts included.
//
// `*q + 10` is always in [10, 265], but the read uses `(signed char)` of it, which is negative
// for sums from 128 to 265. Querying the value before the narrowing cast proves the access in
// bounds and hides a real out-of-bounds read (p[-118..-1]).
//
// This fixture must report in BOTH passes.

#include <stdlib.h>

char read_signed(const unsigned char* q)
{
    char* p = malloc(300);
    if (!p)
        return 0;
    char r = p[(signed char)(*q + 10)];
    free(p);
    return r;
}

// at line 18, column 14
// [ !!Warn ] potential out-of-bounds read on heap buffer 'call' via unchecked index
// ↳ inferred heap capacity: 300 element(s)
// ↳ index value is not proven to be within [0, capacity-1]
