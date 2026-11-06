// SPDX-License-Identifier: Apache-2.0
// i reaches 10 in the loop and the access may run with it, but not on every
// iteration: the report is potential.
void conditional_access_bad(void)
{
    char buf[10];
    for (int i = 0; i <= 10; i++)
        if (i % 2 == 0)
            buf[i] = 'a';
}

// strict-expectation-details: true

// not contains: on every run

// at line 9, column 20
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf
// ↳ index variable may go up to 10 (array last valid index: 9)
// ↳ (this is a write access)
