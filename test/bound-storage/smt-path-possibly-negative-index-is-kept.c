// SPDX-License-Identifier: Apache-2.0
//
// Guard: an index the guards bound from above only stays reported.
//
// `i < n` and `n <= 16` keep `i` below 16, but nothing keeps it at or above 0: i = -100 with
// n = 0 passes every guard and writes before `buf`. The solver must prove the index inside
// both bounds before it removes the report.
//
// This fixture must report in BOTH passes.

void upper_guards_only(int i, int n)
{
    char buf[16];
    if (i > 20)
        return;
    if (i >= n)
        return;
    if (n > 16)
        return;
    buf[i] = 1;
}

// at line 20, column 12
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
// ↳ alias path: buf
// ↳ index variable may go up to 20 (array last valid index: 15)
// ↳ (this is a write access)
