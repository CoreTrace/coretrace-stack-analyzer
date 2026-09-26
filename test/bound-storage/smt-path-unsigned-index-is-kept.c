// SPDX-License-Identifier: Apache-2.0
//
// Guard: the SMT query must bound the index the access really uses, casts included.
//
// `i < n` and `n <= 16` keep `i` below 16, but not above 0: for a negative `i`,
// `(unsigned)i` is far past the end of `buf`. Querying `i` before the cast proves the access
// in bounds and hides the overflow.
//
// This fixture must report in BOTH passes.

void unsigned_index(int i, int n)
{
    char buf[16];
    if (i > 20)
        return;
    if (i >= n)
        return;
    if (n > 16)
        return;
    buf[(unsigned)i] = 1;
}

// at line 20, column 22
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
// ↳ alias path: buf
// ↳ index variable may go up to 4294967295 (array last valid index: 15)
// ↳ (this is a write access)

// strict-expectation-details: true
