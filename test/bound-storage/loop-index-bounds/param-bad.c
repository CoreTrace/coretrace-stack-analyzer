// SPDX-License-Identifier: Apache-2.0
// A trip count bounded by an early return: n <= 11, so i may reach 10. Only a
// caller passing 11 overflows: the report is potential.
void param_bad(int n)
{
    char buf[10];
    if (n > 11)
        return;
    for (int i = 0; i < n; i++)
        buf[i] = 'a';
}

// strict-expectation-details: true

// not contains: on every run

// at line 10, column 16
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf
// ↳ index variable may go up to 10 (array last valid index: 9)
// ↳ (this is a write access)
