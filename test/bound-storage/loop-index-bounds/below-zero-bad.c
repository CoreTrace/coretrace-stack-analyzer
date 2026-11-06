// SPDX-License-Identifier: Apache-2.0
// A loop counting down to -1: its last iteration writes buf[-1] on every run.
void below_zero_bad(void)
{
    char buf[10];
    for (int i = 9; i >= -1; i--)
        buf[i] = 'a';
}

// strict-expectation-details: true

// at line 7, column 16
// [!!] negative index on variable 'buf' (size 10)
// ↳ alias path: buf
// ↳ index reaches -1 on every run of the function that returns
// ↳ (this is a write access)
