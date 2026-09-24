// SPDX-License-Identifier: Apache-2.0
//
// SMT refinement bounds the index the access really uses.
//
// `p[*q]` widens an unsigned char, so the index is in [0, 255] and always inside the 256-byte
// buffer. The default pass reports the read; the solver proves it safe once it sees the
// zero-extension instead of the byte compared as a signed value.

#include <stdlib.h>

char read_byte_index(const unsigned char* q)
{
    char* p = malloc(256);
    if (!p)
        return 0;
    char r = p[*q];
    free(p);
    return r;
}

// [default] at line 16, column 14
// [ !!Warn ] potential out-of-bounds read on heap buffer 'call' via unchecked index
// ↳ inferred heap capacity: 256 element(s)
// ↳ index value is not proven to be within [0, capacity-1]

// [smt-z3] not contains: potential out-of-bounds read on heap buffer
