// SPDX-License-Identifier: Apache-2.0
// A loop counting down from 10: its first iteration writes buf[10] on every run.
void down_bad(void)
{
    char buf[10];
    for (int i = 10; i >= 0; i--)
        buf[i] = 'a';
}

// strict-expectation-details: true

// at line 7, column 16
// [ !!Warn ] stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf
// ↳ index reaches 10 on every run of the function that returns (array last valid index: 9)
// ↳ (this is a write access)
