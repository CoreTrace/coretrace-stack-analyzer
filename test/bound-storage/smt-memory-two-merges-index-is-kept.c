// SPDX-License-Identifier: Apache-2.0
//
// Guard: two values of an index separated by a merge are two values for the solver.
//
// c = 0, b = 15: the first i is 15 and passes `i <= 15`, the second is 16 and writes
// buf[16]. Seen as one value, the first guard would prove the access in bounds.
//
// This fixture must report in BOTH passes.

void two_merges_index(int a, int b, int c)
{
    char buf[16];
    int i;
    if (c)
        i = a;
    else
        i = b;
    if (i < 0 || i > 15)
        return;
    if (c > 1)
        i = a + 1;
    else
        i = b + 1;
    if (i > 20)
        return;
    buf[i] = 1;
}

// at line 26, column 12
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
// ↳ alias path: buf
// ↳ index variable may go up to 20 (array last valid index: 15)
// ↳ (this is a write access)
