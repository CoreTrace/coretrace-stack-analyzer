// SPDX-License-Identifier: Apache-2.0
//
// Guard: the SMT query must bound the size the call really uses, casts included.
//
// `m >= 300` and `n > m` make `n` at least 301, but the length is `(unsigned char)n - 1`:
// n = 512 truncates to 0 and the length wraps. Querying `n` before the narrowing cast proves
// the size above 1 and hides the underflow.
//
// This fixture must report in BOTH passes.

#include <string.h>

void truncated_length(char* out, const char* src, int n, int m)
{
    char dst[256] = {0};
    if (m < 300)
        return;
    if (n <= m)
        return;
    strncpy(dst, src, (unsigned char)n - 1);
    out[0] = dst[0];
}

// at line 20, column 5
// [ !!Warn ] potential unsafe write with length (size - 1) in strncpy
// ↳ size operand may be <= 1
