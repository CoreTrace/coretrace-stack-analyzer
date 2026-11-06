// SPDX-License-Identifier: Apache-2.0
// Two counters: j follows i over 0..10, and reaches 10 on every run.
void two_ivs_bad(void)
{
    char buf[10];
    int j = 0;
    for (int i = 0; i <= 10; i++)
        buf[j++] = 'a';
}

// strict-expectation-details: true

// at line 8, column 18
// [ !!Warn ] stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf
// ↳ index reaches 10 on every run of the function that returns (array last valid index: 9)
// ↳ (this is a write access)
