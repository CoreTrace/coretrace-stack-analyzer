// SPDX-License-Identifier: Apache-2.0
//
// Inside `for (i = I; i != N; i += S)`, the index stops one step before N.

// i is in [0, 15]: in bounds.
void ne_bound_is_size(void)
{
    char buf[16];
    for (int i = 0; i != 16; i++)
        buf[i] = 0;
}

// i is in {0, 2, ..., 14}: in bounds.
void ne_step_two(void)
{
    char buf[16];
    for (int i = 0; i != 16; i += 2)
        buf[i] = 0;
}

// i is in [0, 15], counting down: in bounds.
void ne_down_to_minus_one(void)
{
    char buf[16];
    for (int i = 15; i != -1; i--)
        buf[i] = 0;
}

// i is in [0, 12]: buf[12] is written, and 12 is the largest index.
void ne_one_past(void)
{
    char buf[12];
    for (int i = 0; i != 13; i++)
        buf[i] = 0;
}

// not contains: at line 10, column 16
// not contains: at line 18, column 16
// not contains: at line 26, column 16

// at line 34, column 16
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 12)
// ↳ alias path: buf
// ↳ index variable may go up to 12 (array last valid index: 11)
// ↳ (this is a write access)

// strict-expectation-details: true
